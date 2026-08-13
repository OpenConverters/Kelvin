// The cross-reference worker behind the MCP server's `cross_reference` tool.
//
// Kelvin's substitute ranker is C++ (CrossRef.hpp) and PyKelvin exposes it directly — but the
// ranker only SCORES a candidate list. Turning "this MPN" into a scored list also needs the
// per-family candidate pre-gate and the shard-row -> ranker-spec projection, and those live in
// `web/src/crossref.js`, imported here rather than re-written in Python. That file is explicit
// about why: a second copy drifts within a release, and Qarlos (the standing auditor) audits the
// real pipeline through the same module. The MCP surface must not become the third copy.
//
// So this is a thin worker, wired exactly like tools/qarlos/probe.mjs: the same kelvin.js WASM
// the browser loads, shards read off disk instead of fetched. Requests arrive as one JSON object
// per line on stdin, replies leave as one JSON object per line on stdout:
//
//   {"id":1,"op":"families"}
//   {"id":2,"op":"crossref","family":"capacitor","mpn":"...","manufacturer":"...",
//    "manufacturers":["Vishay"],"sameType":true,"maxResults":12,"poolLimit":800}
//
//   {"id":1,"ok":true,"result":{...}} | {"id":1,"ok":false,"error":"..."}
//
//   node xref.mjs [--shards <dir>]

import { createHash } from 'node:crypto'
import { readFileSync, existsSync, openSync, readSync, closeSync } from 'node:fs'
import { createInterface } from 'node:readline'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const KELVIN = resolve(HERE, '..')

function arg(name, dflt) {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt
}

const SHARDS = resolve(arg('shards', join(KELVIN, 'web', 'public', 'kelvin')))

// ── the engine, wired for Node (identical to probe.mjs's) ────────────────────
const wasm = await import(pathToFileURL(join(KELVIN, 'web', 'public', 'kelvin.js')).href)
const M = await wasm.default()

const loaded = new Set()
function ensureShard(family) {
  if (loaded.has(family)) return
  const p = join(SHARDS, `${family}.kidx`)
  if (!existsSync(p)) {
    throw new Error(`no shard for '${family}' at ${p} — run web/scripts/build-kelvin-shards.sh`)
  }
  const out = M.load_shard(family, readFileSync(p))
  if (typeof out === 'string' && out.startsWith('Exception: ')) {
    throw new Error(`${family}: ${out.slice('Exception: '.length)}`)
  }
  loaded.add(family)
}

function callJson(fn, ...args) {
  const out = M[fn](...args)
  if (typeof out === 'string' && out.startsWith('Exception: ')) {
    throw new Error(out.slice('Exception: '.length))
  }
  return JSON.parse(out)
}

// The RAW catalogue record behind a shard row, by byte span — the same Range fetch the site's
// part drawer does. Returned for the ORIGINAL only: it is where the evidence the shard does not
// carry lives (a connector's mating series, a capacitor's datasheet URL), and an FAE reading a
// substitution needs to see it. The capacitor's X1/X2 safety series is no longer among those —
// it travels on the shard row as `family` and the ranker gates on it (ABT #557).
function rawRecord(family, row) {
  if (typeof row?.srcOffset !== 'number' || typeof row?.srcLength !== 'number') return null
  const p = join(SHARDS, `${family}.ndjson`)
  if (!existsSync(p)) return null
  const fd = openSync(p, 'r')
  try {
    const buf = Buffer.alloc(row.srcLength)
    readSync(fd, buf, 0, row.srcLength, row.srcOffset)
    return JSON.parse(buf.toString('utf8'))
  } catch { return null } finally { closeSync(fd) }
}

const engine = {
  async browse(family, query = {}) {
    ensureShard(family)
    return callJson('browse', family, JSON.stringify(query))
  },
  async crossReference(category, original, candidates, options = {}) {
    const out = callJson('cross_reference_string', category, JSON.stringify(original),
      JSON.stringify(candidates), JSON.stringify(options))
    if (out?.error) throw new Error(`cross-reference failed: ${out.error}`)
    return out
  },
}

const { XREF, famFor, originalMissingKeys, runCrossRef, POOL_LIMIT } = await import(
  pathToFileURL(join(KELVIN, 'web', 'src', 'crossref.js')).href)

// ── operations ──────────────────────────────────────────────────────────────

// Which families the ranker has a parameter model for — read off XREF, not listed again here,
// so a family added to the web pipeline reaches the MCP surface with it.
function families() {
  return {
    families: XREF.map((f) => ({
      key: f.key, label: f.label, category: f.category,
      primary: f.primary ? { label: f.primary.label, unit: f.primary.unit } : null,
      hardGates: f.hardKeys, caveat: f.caveat ?? null,
    })),
  }
}

// Resolve "this MPN" to exactly one catalogue row. An MPN substring can hit many parts and two
// vendors can ship the same MPN string, so an ambiguous request comes back as the candidate
// list to disambiguate with — never as a silently chosen first row.
async function findOriginal(family, mpn, manufacturer) {
  const page = await engine.browse(family, { filters: { mpn }, limit: 50,
                                             sort: { field: 'lineno', dir: 'asc' } })
  let rows = page.rows
  if (manufacturer) {
    const want = manufacturer.toLowerCase()
    rows = rows.filter((r) => (r.manufacturer ?? '').toLowerCase().includes(want))
  }
  if (!rows.length) {
    throw new Error(`no ${family} in the catalogue whose MPN contains '${mpn}'` +
      (manufacturer ? ` from a manufacturer matching '${manufacturer}'` : '') +
      ` (${page.total} row(s) matched the MPN filter before the manufacturer filter)`)
  }
  const exact = rows.filter((r) => r.mpn.toLowerCase() === mpn.toLowerCase())
  const pool = exact.length ? exact : rows
  if (pool.length > 1) {
    const err = new Error(
      `'${mpn}' matches ${pool.length} ${family} rows — name the manufacturer, or use the exact ` +
      `MPN: ${pool.slice(0, 10).map((r) => `${r.mpn} (${r.manufacturer})`).join(', ')}`)
    err.choices = pool.slice(0, 10).map((r) => ({ mpn: r.mpn, manufacturer: r.manufacturer }))
    throw err
  }
  return pool[0]
}

async function crossref(req) {
  const { family, mpn } = req
  if (!XREF.some((f) => f.key === family)) {
    throw new Error(`no cross-reference model for '${family}' — the ranker covers ` +
      `${XREF.map((f) => f.key).join(', ')}`)
  }
  if (!mpn) throw new Error('cross_reference needs an mpn')
  const original = await findOriginal(family, mpn, req.manufacturer)

  // Target vendors: the caller's list, or every other vendor that actually has parts in this
  // family (the facet), which is what "find me a second source" means. Facets are counted over
  // the whole family, so the list is the catalogue's, not a hardcoded one.
  let manufacturers = (req.manufacturers ?? []).filter(Boolean)
  let targetsFromFacet = false
  if (!manufacturers.length) {
    const facets = await engine.browse(family, { withFacets: true, facetTop: 500, limit: 0 })
    manufacturers = (facets.facets?.manufacturer?.values ?? [])
      .map((x) => (Array.isArray(x) ? x[0] : x?.value))
      .filter((m) => m && m !== original.manufacturer)
    targetsFromFacet = true
    if (!manufacturers.length) {
      throw new Error(`the ${family} catalogue has no vendor other than ${original.manufacturer}`)
    }
  }

  const fam = famFor(family, original)
  const poolLimit = Number(req.poolLimit ?? POOL_LIMIT)
  const r = await runCrossRef({
    family, original, manufacturers,
    sameType: req.sameType !== false,
    maxResults: Number(req.maxResults ?? 12),
    poolLimit, engine,
  })
  return {
    family, category: fam.category, caveat: fam.caveat ?? null,
    original, originalRaw: rawRecord(family, original),
    origSpec: r.origSpec, origVerified: r.origVerified,
    // `missing` is the display wording; `missingKeys` is what a consumer matches on —
    // a spec key survives someone improving the comparison table, a label does not.
    missing: r.missing, missingKeys: originalMissingKeys(fam, r.origSpec),
    targets: manufacturers, targetsFromFacet,
    poolTotal: r.poolTotal, poolScored: r.poolScored, poolLimit,
    // Each candidate carries BOTH its shard row (the raw catalogue datum) and `specs`: the
    // exact projection the ranker compared against the original, built by the family's own
    // spec() — the same function that produced origSpec. That vocabulary matters. The row
    // says `vds_rated` / `id_continuous` / `qg_total`; the ranker says `vds` / `id` / `qg`,
    // and so does origSpec. A consumer tabulating candidates against the original needs the
    // two sides in one vocabulary, and it must not have to re-derive the mapping.
    ranked: r.ranked.map((c) => {
      const row = r.rowByKey.get(c._key) ?? null
      const { _key, ...specs } = row ? fam.spec(row) : {}
      return { ...c, row, specs }
    }),
  }
}

// ── the loop ────────────────────────────────────────────────────────────────
const reply = (o) => process.stdout.write(JSON.stringify(o) + '\n')

// The fingerprint of the code this worker is actually RUNNING — its own source and the
// cross-reference pipeline it imports. A worker is long-lived and is restarted only when it
// dies, so an edit to either file would otherwise keep taking effect "next week": the server
// holds a process from before the change and every answer stays quietly stale. The server
// compares this against the files on disk and restarts the worker when they differ.
export function sourceFingerprint() {
  // Must list exactly what server.py lists, in the same order — the two hashes are compared
  // against each other, so a file added on one side only reads as a mid-start race.
  const files = [join(HERE, 'xref.mjs'),
                 join(KELVIN, 'web', 'src', 'crossref.js'),
                 join(KELVIN, 'web', 'public', 'kelvin.js')]
  const h = createHash('sha256')
  for (const f of files) h.update(readFileSync(f))
  return h.digest('hex').slice(0, 16)
}

const rl = createInterface({ input: process.stdin })
const fingerprint = sourceFingerprint()
process.stderr.write(`kelvin xref worker ready (shards ${SHARDS}, source ${fingerprint})\n`)
reply({ ready: true, fingerprint })

for await (const line of rl) {
  if (!line.trim()) continue
  let req
  try {
    req = JSON.parse(line)
  } catch (e) {
    reply({ id: null, ok: false, error: `bad request json: ${e.message}` })
    continue
  }
  try {
    const result = req.op === 'families' ? families()
      : req.op === 'crossref' ? await crossref(req)
      : (() => { throw new Error(`unknown op '${req.op}'`) })()
    reply({ id: req.id ?? null, ok: true, result })
  } catch (e) {
    reply({ id: req.id ?? null, ok: false, error: String(e.message || e),
            choices: e.choices ?? null })
  }
}
