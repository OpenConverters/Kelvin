// Qarlos' hands: run Kelvin's REAL cross-reference headlessly.
//
// This is not a re-implementation of the cross-ref pipeline — it imports
// web/src/crossref.js, the same module the site's CrossRefView renders from, and
// runs it against the same kelvin.js WASM the browser loads. Only the transport
// differs: shards are read off disk instead of fetched, and the module is loaded
// in-process instead of in a Worker. An auditor checking a second copy of the
// ranking rules would be auditing a fiction, so there is no second copy.
//
// Emits ONE json line per probed part on stdout:
//   {family, category, original:{...}, targets:[mfr], poolTotal, poolScored,
//    origVerified, missing:[...], caveat, ranked:[{mpn, manufacturer, verdict,
//    score, params:[{name, verdict}], ...}]}
//
//   node probe.mjs --per-family 2 [--seed 12345] [--families mosfet,capacitor]
//                  [--max-results 10] [--shards <dir>]

import { readFileSync, existsSync, openSync, readSync, closeSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const KELVIN = resolve(HERE, '..', '..')

function arg(name, dflt) {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt
}

const SHARDS = resolve(arg('shards', join(KELVIN, 'web', 'public', 'kelvin')))
const PER_FAMILY = Number(arg('per-family', 2))
const MAX_RESULTS = Number(arg('max-results', 10))
const SEED = Number(arg('seed', 0)) || (Date.now() & 0x7fffffff)

// Deterministic PRNG so a reported finding can be reproduced exactly by rerunning
// with the same --seed. Math.random would make every finding unrepeatable.
let _s = SEED >>> 0
function rnd() {
  _s ^= _s << 13; _s >>>= 0
  _s ^= _s >> 17
  _s ^= _s << 5; _s >>>= 0
  return _s / 4294967296
}
const pick = (a) => a[Math.floor(rnd() * a.length)]

// ── the engine, wired for Node ───────────────────────────────────────────────
const wasm = await import(pathToFileURL(join(KELVIN, 'web', 'public', 'kelvin.js')).href)
const M = await wasm.default()

const loaded = new Set()
function ensureShard(family) {
  if (loaded.has(family)) return
  const p = join(SHARDS, `${family}.kidx`)
  if (!existsSync(p)) throw new Error(`no shard for '${family}' at ${p} — run web/scripts/build-kelvin-shards.sh`)
  const out = M.load_shard(family, readFileSync(p))
  if (typeof out === 'string' && out.startsWith('Exception: ')) {
    throw new Error(`${family}: ${out.slice(11)}`)
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

// The RAW catalogue record behind a shard row, by byte span — the same Range fetch
// the site's part drawer does. This is what makes the audit able to tell three
// different faults apart, instead of lumping them together as "the tool is wrong":
//   raw and shard row DISAGREE      -> Kelvin extracted the record wrongly
//   they agree but the value is bad -> the TAS catalogue is wrong
//   both fine, verdict still wrong  -> the ranking logic is wrong
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

const { XREF, famFor, runCrossRef } = await import(
  pathToFileURL(join(KELVIN, 'web', 'src', 'crossref.js')).href)

// ── probe ────────────────────────────────────────────────────────────────────
const only = (arg('families', '') || '').split(',').filter(Boolean)
const families = XREF.filter((f) => !only.length || only.includes(f.key))

process.stderr.write(`qarlos probe: seed=${SEED} families=${families.map((f) => f.key).join(',')}\n`)

for (const f of families) {
  for (let n = 0; n < PER_FAMILY; n++) {
    try {
      // A random ORIGINAL, drawn from the whole shard so the audit is not biased
      // to whatever sorts first. Take a random offset into the family's rows.
      const head = await engine.browse(f.key, { limit: 1 })
      const total = head.total ?? 0
      if (!total) { process.stderr.write(`${f.key}: empty shard, skipped\n`); break }
      const offset = Math.floor(rnd() * total)
      const one = await engine.browse(f.key, { limit: 1, offset,
        sort: { field: 'lineno', dir: 'asc' } })
      const original = one.rows?.[0]
      if (!original) { process.stderr.write(`${f.key}: no row at ${offset}\n`); continue }

      // Cross-ref needs a target manufacturer. Take the biggest OTHER vendor in
      // the family — that is what a user marking "find me a second source" does.
      // Same request the view makes (withFacets/facetTop), same response shape:
      // facets.manufacturer.values is [[name, count], ...].
      const facets = await engine.browse(f.key, { withFacets: true, facetTop: 500, limit: 0 })
      const mfrs = (facets.facets?.manufacturer?.values ?? [])
        .map((x) => (Array.isArray(x) ? x[0] : x?.value))
        .filter((m) => m && m !== original.manufacturer)
      if (!mfrs.length) { process.stderr.write(`${f.key}: single-vendor catalogue, skipped\n`); break }
      // Mark the largest few other vendors, which is what the view's "mark all
      // others" does. Marking ONE random vendor mostly yields an empty pool (EPC
      // has no 650 V silicon MOSFET), which audits nothing: the interesting
      // question is whether the parts it DOES return are defensible.
      const targets = mfrs.slice(0, 5)

      const fam = famFor(f.key, original)
      const r = await runCrossRef({
        family: f.key, original, manufacturers: targets,
        sameType: true, maxResults: MAX_RESULTS, engine,
      })
      process.stdout.write(JSON.stringify({
        seed: SEED, family: f.key, category: fam.category, caveat: fam.caveat ?? null,
        original, targets, poolTotal: r.poolTotal, poolScored: r.poolScored,
        origVerified: r.origVerified, missing: r.missing,
        origSpec: r.origSpec,
        originalRaw: rawRecord(f.key, original),
        ranked: r.ranked.map((c) => {
          const row = r.rowByKey.get(c._key) ?? null
          return { ...c, _row: row, _raw: row ? rawRecord(f.key, row) : null }
        }),
      }) + '\n')
    } catch (e) {
      process.stdout.write(JSON.stringify({
        seed: SEED, family: f.key, error: String(e.message || e),
      }) + '\n')
    }
  }
}
