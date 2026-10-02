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
import { readFileSync, existsSync, openSync, readSync, closeSync, readdirSync } from 'node:fs'
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

// Every vendor with parts in a family, from the family's own facet — the catalogue's list, not
// a hardcoded one. Cached: the facet is a whole-family count and does not change per request.
const makersCache = new Map()
async function makersOf(family) {
  if (!makersCache.has(family)) {
    const facets = await engine.browse(family, { withFacets: true, facetTop: 500, limit: 0 })
    makersCache.set(family, (facets.facets?.manufacturer?.values ?? [])
      .map((x) => (Array.isArray(x) ? x[0] : x?.value)).filter(Boolean))
  }
  return makersCache.get(family)
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
  // family (the facet), which is what "find me a second source" means.
  let manufacturers = (req.manufacturers ?? []).filter(Boolean)
  let targetsFromFacet = false
  if (!manufacturers.length) {
    manufacturers = (await makersOf(family)).filter((m) => m !== original.manufacturer)
    targetsFromFacet = true
    if (!manufacturers.length) {
      throw new Error(`the ${family} catalogue has no vendor other than ${original.manufacturer}`)
    }
  }
  return rank(family, original, manufacturers, targetsFromFacet, req)
}

// The cross-reference proper, for an original already resolved to one catalogue row: the
// web app's runCrossRef, and the candidate projection described below. Shared by
// `crossref` (one MPN) and `bom` (every line of a bill of materials).
async function rank(family, original, manufacturers, targetsFromFacet, req) {
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

// ── a bill of materials: identify every line, then cross-reference it ───────────
// The `bom` op behind the MCP server's crossref_bom. A BOM line says what the part is in
// whatever words the exporting tool chose — a part-number column, a value, a footprint name,
// a description — and the catalogue has to be ASKED which part that is before anything can be
// ranked. The ranking is runCrossRef above, unchanged. What is new here is identification, and
// it follows the Faraday web app's parts.js (identify / candidatesByValue / packageOf), which
// answers the same question for the parts on a board:
//
//   - part number first, across every family: the families the designator prefix, the value's
//     unit and the footprint name suggest are searched FIRST, then all the rest. A prefix is a
//     convention, not a contract, so it orders the search and never excludes a family.
//   - a hit is EXACT when the catalogue MPN equals the BOM's (case-insensitive), NORMALISED
//     when they differ only in punctuation or spacing, and otherwise only a SUBSTRING hit,
//     which identifies nothing and is listed for the reader rather than ranked.
//   - no part number, or none the catalogue carries: the value and the package, within the
//     family the value's unit names. That lists the catalogue parts the line MIGHT be; it
//     does not pick one, because the BOM does not say which.
//
// Only an exactly identified line is cross-referenced: the ranker needs ONE original, and a
// line matched by value is a set of them.
//
// parts.js is Faraday's and Faraday imports Kelvin, not the other way round, so the few
// functions this needs are restated here rather than imported. They are the identification
// heuristics only — nothing here scores a candidate.

const fold = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '')
  .toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim()
const canon = (s) => String(s || '').toUpperCase().replace(/[^A-Z0-9]/g, '')

// Every family the catalogue holds, read off the shard directory — the families this worker
// can actually open, which is what "search every family" has to mean.
function allFamilies() {
  return readdirSync(SHARDS).filter((f) => f.endsWith('.kidx')).map((f) => f.slice(0, -5)).sort()
}

// The value string: "100n", "4u7", "2k2", "3R3", "0.1uF", "2.2µF 16V X7R", "10uF/25V", "1M".
// (parts.js parseValue.) Without a unit letter the kind comes from the designator, because
// 100n is 100 nF on a capacitor and 100 nH on an inductor; a bare number is refused.
const PREFIX = { p: 1e-12, n: 1e-9, u: 1e-6, m: 1e-3, k: 1e3, K: 1e3, M: 1e6, G: 1e9, '': 1 }
const KIND_UNIT = { C: 'F', R: 'Ω', L: 'H' }

function parseValue(raw, kindHint = null) {
  if (!raw) return null
  const s = String(raw).replace(/[µμ]/g, 'u').replace(/[Ωω]|ohms?/gi, 'R')
  const fields = s.split(/[\s,/;]+/).filter(Boolean)
  if (!fields.length) return null
  const first = fields[0]
  let num = null, mult = null, unit = null
  let m = first.match(/^(\d+)([pnumkKMGR])(\d*)$/)
  if (m) {
    num = Number(m[1] + (m[3] ? '.' + m[3] : ''))
    if (m[2] === 'R') { mult = 1; unit = 'R' } else mult = PREFIX[m[2]]
  } else {
    m = first.match(/^(\d+(?:\.\d+)?)([pnumkKMG]?)([FHR]?)$/)
    if (!m) return null
    num = Number(m[1]); mult = PREFIX[m[2]]; unit = m[3] || null
    if (!m[2] && !m[3]) {
      const two = fields[1]?.match(/^([pnumkKMG]?)([FHR])$/)
      if (two) { mult = PREFIX[two[1]]; unit = two[2] }
      else if (kindHint !== 'R') return null
      else mult = 1
    }
  }
  const kind = unit === 'F' ? 'C' : unit === 'H' ? 'L' : unit === 'R' ? 'R'
    : (mult >= 1e3 ? 'R' : kindHint)
  if (!kind) return null
  let ratedV = null
  for (const f of fields.slice(1)) {
    const v = f.match(/^(\d+(?:\.\d+)?)\s*V(?:DC|AC)?$/i)
    if (v) { ratedV = Number(v[1]); break }
  }
  return { kind, si: num * mult, unit: KIND_UNIT[kind], ratedV, raw: String(raw) }
}

// A description is prose ("CAP CER 0.1UF 16V X7R 0402"), so only a token carrying its own
// unit letter is read from it — a bare "100" in a description is never a value.
function valueFromDescription(desc) {
  if (!desc) return null
  const tokens = String(desc).replace(/[µμ]/g, 'u').split(/[\s,;/()]+/).filter(Boolean)
  for (let i = 0; i < tokens.length; i++) {
    const t = tokens[i]
    if (!/^\d+(?:\.\d+)?[pnumkKMG]?(F|H|R|Ω|ohms?)$/i.test(t) && !/^\d+[pnumkKMGR]\d+$/.test(t)) continue
    // "F"/"H" only in upper case or with a prefix: a lone "1h" is not a henry
    const v = parseValue([t, ...tokens.slice(i + 1)].join(' '))
    if (v) return v
  }
  return null
}

// The footprint name (parts.js packageOf), with one correction: a case code written as
// "1005M/0402" names the SAME part twice, metric then imperial, so a run marked metric is
// converted rather than read as an imperial code.
const METRIC_TO_IMPERIAL = {
  '0603': '0201', '1005': '0402', '1608': '0603', '2012': '0805', '3216': '1206',
  '3225': '1210', '4516': '1806', '4532': '1812', '5025': '2010', '5750': '2220',
  '6332': '2512',
}
const IMPERIAL = new Set(Object.values(METRIC_TO_IMPERIAL))
const PKG_RE = /(SOT-?\d+(?:-\d+)?|SOD-?\d+|TO-?\d+(?:-\d+)?|D2?PAK|DPAK|SMA|SMB|SMC|DO-?\d+[A-Z]*|SOIC-?\d+|SO-?\d+|TSSOP-?\d*|MSOP-?\d*|SSOP-?\d*|QFN-?\d*|DFN-?\d*|LQFP-?\d*|TQFP-?\d*|QFP-?\d*|BGA|WSON|PowerPAK|LFPAK|SIP-?\d*|DIP-?\d+)/i
const NOT_A_PART = /mounting|hole|fiducial|logo|testpoint|test_point|tp_|symbol|marking|net[-_ ]?tie/i

function footprintName(fp) {
  if (!fp) return ''
  const i = fp.indexOf(':')
  return i >= 0 ? fp.slice(i + 1) : fp
}

function chipCode(text) {
  let metric = null
  for (const g of String(text).matchAll(/(?:^|[^0-9])(\d{4})(?![0-9])(M(?:etric)?\b)?/gi)) {
    if (g[2]) { if (!metric && METRIC_TO_IMPERIAL[g[1]]) metric = METRIC_TO_IMPERIAL[g[1]]; continue }
    if (IMPERIAL.has(g[1])) return g[1]
    if (!metric && METRIC_TO_IMPERIAL[g[1]]) metric = METRIC_TO_IMPERIAL[g[1]]
  }
  return metric
}

function packageOf(text) {
  const name = footprintName(text)
  if (!name) return null
  const ipc = name.match(/^(?:CAP|RES|IND|LED|DIO)[A-Z]*(\d{4})X/i)
  if (ipc && METRIC_TO_IMPERIAL[ipc[1]]) return { code: METRIC_TO_IMPERIAL[ipc[1]], kind: 'chip' }
  const chip = chipCode(name)
  if (chip) return { code: chip, kind: 'chip' }
  const m = name.match(PKG_RE)
  if (m) return { code: m[1].toUpperCase().replace(/[-\s]/g, ''), kind: 'pkg' }
  return null
}

function rowPackage(row) {
  const c = row?.caseCode
  if (!c) return null
  const chip = chipCode(c)
  if (chip) return { code: chip, kind: 'chip' }
  const m = c.match(PKG_RE)
  if (m) return { code: m[1].toUpperCase().replace(/[-\s]/g, ''), kind: 'pkg' }
  return { code: c.toUpperCase().replace(/[-\s]/g, ''), kind: 'other' }
}

// EIA chip bodies in mm and the per-axis tolerance that keeps neighbours apart (parts.js).
const CHIP_MM = {
  '0201': [0.60, 0.30], '0402': [1.00, 0.50], '0603': [1.60, 0.80],
  '0805': [2.00, 1.25], '1206': [3.20, 1.60], '1210': [3.20, 2.50],
  '1806': [4.50, 1.60], '1812': [4.50, 3.20], '2010': [5.00, 2.50],
  '2220': [5.70, 5.00], '2512': [6.30, 3.20],
}
const CHIP_TOL = 0.12

function rowSize(row) {
  const l = row?.lengthM, w = row?.widthM
  if (!(l > 0) || !(w > 0)) return null
  const [a, b] = [l * 1000, w * 1000].sort((x, y) => y - x)
  for (const [code, [nl, nw]] of Object.entries(CHIP_MM)) {
    if (Math.abs(a - nl) <= CHIP_TOL * nl && Math.abs(b - nw) <= CHIP_TOL * nw) return code
  }
  return ''          // measured, and no standard chip
}

// Does a catalogue row fit the package the BOM names? The measured body decides for a chip
// (a case string is ambiguous between metric and imperial; a drawing is not); the case code
// decides otherwise. 'unknown' when the row says neither — never counted as a fit.
function packageMatches(pkg, row) {
  if (!pkg) return 'unknown'
  if (pkg.kind === 'chip') {
    const size = rowSize(row)
    if (size != null) return size === pkg.code ? 'match' : 'differs'
  }
  const rp = rowPackage(row)
  if (!rp) return 'unknown'
  if (pkg.kind === 'chip') return rp.kind === 'chip' && rp.code === pkg.code ? 'match' : 'differs'
  return rp.code.startsWith(pkg.code) || pkg.code.startsWith(rp.code) ? 'match' : 'differs'
}

// Which families to open first (parts.js familyOrder). ORDER only.
const BY_PREFIX = [
  [/^C[A-Z]?\d/i, ['capacitor']],
  [/^R[A-Z]?\d/i, ['resistor']],
  [/^(RV|VR|MOV|VDR|ZNR)\d/i, ['varistor']],
  [/^(L|FB|FL|T|TR|TX)\d/i, ['magnetic']],
  [/^Q\d/i, ['mosfet', 'bjt', 'igbt']],
  [/^(D|LED|TVS|Z)\d/i, ['diode']],
  [/^(U|IC)\d/i, ['controller', 'analog']],
  [/^(Y|X|XTAL|OSC)\d/i, ['timing']],
  [/^(J|P|CN|CON|K|X)\d/i, ['connector']],
]
const BY_FOOTPRINT = [
  [/^(C_|CAPC|CAP|CP_)|Capacitor|Elko/i, ['capacitor']],
  [/^(R_|RESC|RES)|Resistor/i, ['resistor']],
  [/^(L_|IND)|Inductor|Choke|Transformer|Bead|Ferrite/i, ['magnetic']],
  [/Varistor|MOV/i, ['varistor']],
  [/Crystal|Oscillator|XTAL|Resonator/i, ['timing']],
  [/Conn|Header|Terminal|Socket|USB|RJ\d|JST|Molex|Phoenix|MKDS|Screw|BNC/i, ['connector']],
  [/SOD|\bSMA\b|\bSMB\b|\bSMC\b|DO-?\d|Diode|^D_/i, ['diode']],
  [/SOT|TO-?\d|D2?PAK|DFN|PowerPAK|LFPAK|SO-?8|SOIC-?8/i, ['mosfet', 'diode', 'bjt', 'igbt']],
  [/QFN|QFP|TSSOP|MSOP|SSOP|BGA|SOIC|SOP|DIP/i, ['controller', 'analog']],
]
const BY_KIND = { C: ['capacitor'], R: ['resistor'], L: ['magnetic'] }
const VALUE_FIELD = { C: { family: 'capacitor', field: 'capacitance', tol: 0.02 },
                      R: { family: 'resistor', field: 'resistance', tol: 0.005 },
                      L: { family: 'magnetic', field: 'inductance', tol: 0.02 } }

function refdesKind(ref) {
  if (/^C[A-Z]?\d/i.test(ref || '')) return 'C'
  if (/^R[A-Z]?\d/i.test(ref || '')) return 'R'
  if (/^(L|FB|FL)\d/i.test(ref || '')) return 'L'
  return null
}

function familyOrder(line, value) {
  const out = []
  const add = (fams) => { for (const f of fams) if (!out.includes(f)) out.push(f) }
  if (value) add(BY_KIND[value.kind])
  const fn = footprintName(line.footprint)
  for (const [re, fams] of BY_FOOTPRINT) if (re.test(fn)) add(fams)
  for (const [re, fams] of BY_PREFIX) if (re.test(line.ref || '')) add(fams)
  return out
}

// The strings that might be the part number, most trusted first. A cell the BOM labels as a
// part number is believed on that say-so — an all-digit ordering code (Würth's 885012206095)
// is kept — and when it reads "<maker> <number>" ("ST USBLC6-2SC6", "Phoenix 1725656 (MPT
// 0,5/2-2,54)") the number is tried on its own as well, with the leading word kept as a hint
// to the maker. A token scraped from the value column has to LOOK like a part number, because
// there most digit runs are a value.
function mpnCandidates(line) {
  const out = []
  let makerHint = null
  const push = (s, { stated = false } = {}) => {
    const t = String(s || '').trim()
    if (!t || t.length < 4 || !/\d/.test(t)) return
    if (!stated) {
      if (!/[A-Za-z]/.test(t)) return
      if (/^\d+(?:\.\d+)?[pnumkKMGR]?\d*[FHRV]?$/i.test(t)) return
      if (parseValue(t, refdesKind(line.ref))) return
    }
    if (NOT_A_PART.test(t)) return
    if (!out.includes(t)) out.push(t)
  }
  const cell = String(line.partNumber || '').trim()
  if (cell) {
    push(cell, { stated: true })
    const words = cell.replace(/\([^)]*\)/g, ' ').split(/\s+/).filter(Boolean)
    if (words.length > 1 && !/\d/.test(words[0])) {
      makerHint = words[0]
      push(words.slice(1).join(' '), { stated: true })
    }
    if (words.length > 1) for (const w of words) push(w, { stated: true })
  }
  const value = String(line.value || '').trim().split(/\s+/)[0]
  push(value)
  return { cands: out, makerHint }
}

// Every catalogue row whose MPN contains `q`, paged: an exact row can sort after 25 substring
// hits ("ABAT54…" before "BAT54"), and a first page alone would call it absent.
const MPN_SCAN_CAP = 5000
async function mpnRows(family, q) {
  const rows = []
  let total = 0
  for (let offset = 0; offset < MPN_SCAN_CAP; offset += 500) {
    const page = await engine.browse(family, { filters: { mpn: q }, sort: { field: 'mpn', dir: 'asc' },
                                               limit: 500, offset })
    total = page.total
    rows.push(...page.rows)
    if (rows.length >= total || !page.rows.length) break
  }
  return { rows, total, truncated: rows.length < total }
}

const makerAgrees = (row, hint) => {
  if (!hint) return true
  const a = fold(row.manufacturer), b = fold(hint)
  return !!a && !!b && (a.includes(b) || b.includes(a) || a.split(' ')[0] === b.split(' ')[0])
}

// Target vendors named by a person ("Würth", "wurth elektronik") against the family's own
// spelling ("Würth Elektronik"): case and accents ignored, exact first, otherwise every
// catalogue name containing it. A name matching nothing is reported, never guessed.
function resolveTargets(wanted, names) {
  const used = [], unknown = []
  for (const w of wanted) {
    const fw = fold(w)
    const exact = names.filter((n) => fold(n) === fw)
    const hits = exact.length ? exact : names.filter((n) => fw && fold(n).includes(fw))
    if (hits.length) for (const h of hits) { if (!used.includes(h)) used.push(h) }
    else unknown.push(w)
  }
  return { used, unknown }
}

function brief(row) {
  if (!row) return null
  const { srcOffset, srcLength, lineno, line, ...rest } = row
  return rest
}

async function crossrefLine(family, original, req) {
  if (!XREF.some((f) => f.key === family)) {
    // famFor() falls back to the FIRST model for a family it does not know, which would rank
    // a controller as if it were a magnetic. Refused, by name.
    return { skipped: 'no-model', why: `Kelvin has no cross-reference model for the ${family} ` +
      `family (it ranks ${XREF.map((f) => f.key).join(', ')})` }
  }
  const names = await makersOf(family)
  const wanted = (req.targets ?? []).filter(Boolean)
  if (wanted.length) {
    const { used, unknown } = resolveTargets(wanted, names)
    if (used.includes(original.manufacturer)) {
      return { already: true, targets: used, unknownTargets: unknown }
    }
    if (!used.length) {
      return { skipped: 'no-target-maker', unknownTargets: unknown,
               why: `none of ${wanted.join(', ')} makes ${family} parts in the catalogue` }
    }
    return { ...(await rank(family, original, used, false, req)), unknownTargets: unknown }
  }
  const others = names.filter((m) => m !== original.manufacturer)
  if (!others.length) {
    return { skipped: 'no-target-maker',
             why: `the ${family} catalogue has no vendor other than ${original.manufacturer}` }
  }
  return rank(family, original, others, true, req)
}

// The catalogue parts of this value that fit this package. Paged to the end of the value
// window (capped, and the cap is reported): taking the first page only would answer from
// whichever vendor's file happens to be indexed first.
const VALUE_SCAN_CAP = 20000
async function byValue(value, pkg, req) {
  const spec = VALUE_FIELD[value.kind]
  if (!spec) return null
  const filters = { [spec.field]: { min: value.si * (1 - spec.tol), max: value.si * (1 + spec.tol) } }
  if (value.kind === 'C' && value.ratedV) filters.v_rated = { min: value.ratedV }
  const targets = (req.targets ?? []).length
    ? resolveTargets(req.targets, await makersOf(spec.family)).used : []
  const matched = [], unknownCase = [], differs = []
  let total = 0, scanned = 0
  for (let offset = 0; offset < VALUE_SCAN_CAP; offset += 1000) {
    const page = await engine.browse(spec.family, { filters, sort: { field: 'lineno', dir: 'asc' },
                                                    limit: 1000, offset })
    total = page.total
    for (const r of page.rows) {
      const v = packageMatches(pkg, r)
      ;(v === 'match' ? matched : v === 'unknown' ? unknownCase : differs).push(r)
    }
    scanned += page.rows.length
    if (scanned >= total || !page.rows.length) break
  }
  // Ordered, never filtered: the target vendors' parts first, because they are what the
  // caller asked for; everyone else's after them.
  const isTarget = (r) => targets.includes(r.manufacturer)
  const ordered = [...matched.filter(isTarget), ...matched.filter((r) => !isTarget(r))]
  return { family: spec.family, field: spec.field, tol: spec.tol, total, scanned,
           truncated: scanned < total, matched: matched.length,
           matchedTarget: matched.filter(isTarget).length,
           unknownCase: unknownCase.length, differs: differs.length,
           rows: ordered.slice(0, Number(req.listed)).map(brief) }
}

async function identifyLine(line, req, families) {
  const kindHint = refdesKind(line.ref)
  let value = parseValue(line.value, kindHint)
  let valueFrom = value ? 'value' : null
  // A description is read for a value only on a line whose designator names a passive, and
  // only when the two agree: "50R right-angle BNC jack" on J6 is a characteristic impedance.
  if (!value && kindHint) {
    const d = valueFromDescription(line.description)
    if (d && d.kind === kindHint) { value = d; valueFrom = 'description' }
  }
  let pkg = packageOf(line.footprint), pkgFrom = pkg ? 'footprint' : null
  if (!pkg && line.description) { pkg = packageOf(line.description); if (pkg) pkgFrom = 'description' }
  const { cands, makerHint } = mpnCandidates(line)
  const order = familyOrder(line, value)
  const searchOrder = [...order, ...families.filter((f) => !order.includes(f))]
  const out = { tried: cands, families: order, makerHint }
  if (value) out.parsedValue = { kind: value.kind, si: value.si, unit: value.unit,
                                 ratedV: value.ratedV, from: valueFrom }
  if (pkg) out.package = { code: pkg.code, from: pkgFrom }
  if (NOT_A_PART.test(`${line.value || ''} ${line.partNumber || ''} ${footprintName(line.footprint)}`)) {
    return { ...out, match: 'not-a-part',
             why: 'its value, part number or footprint names a mounting hole, fiducial, test ' +
                  'point, logo or similar — nothing a catalogue sells' }
  }
  if (!cands.length && !value) {
    return { ...out, match: 'unlookupable',
             why: 'the BOM gives neither a part number nor a value this tool can read' }
  }

  const hint = line.manufacturer || makerHint
  const near = []
  let nearTotal = 0, truncated = false
  for (const family of cands.length ? searchOrder : []) {
    for (const q of cands) {
      const r = await mpnRows(family, q)
      truncated ||= r.truncated
      const exact = r.rows.filter((row) => row.mpn.toLowerCase() === q.toLowerCase())
      const normalised = exact.length ? [] : r.rows.filter((row) => canon(row.mpn) === canon(q))
      const hits = exact.length ? exact : normalised
      if (hits.length) {
        const agreeing = hits.filter((row) => makerAgrees(row, hint))
        const pool = agreeing.length ? agreeing : hits
        const base = { ...out, family, query: q, normalised: !exact.length,
                       outsideSuggestedFamilies: !order.includes(family) }
        if (pool.length > 1) {
          return { ...base, match: 'ambiguous', exactRows: pool.slice(0, Number(req.listed)).map(brief),
                   exactTotal: pool.length,
                   why: `${pool.length} ${family} parts carry the part number '${q}' ` +
                        `(${pool.slice(0, 4).map((x) => x.manufacturer).join(', ')}) and the BOM ` +
                        `does not say which maker it means` }
        }
        const original = pool[0]
        const res = { ...base, match: 'exact', original: brief(original),
                      makerDisagrees: hint && !agreeing.length ? hint : null }
        try {
          res.xref = await crossrefLine(family, original, req)
        } catch (e) {
          res.xrefError = String(e.message || e)
        }
        return res
      }
      // A partial hit is worth showing only when it could be the same part: an all-digit
      // query must START the catalogue MPN (885012206095 -> 885012206095R), because a digit
      // run inside an unrelated MPN (1725656 in MAL217256562E3) is coincidence.
      const digitsOnly = /^\d+$/.test(canon(q))
      const plausible = r.rows.filter((row) => !digitsOnly || canon(row.mpn).startsWith(canon(q)))
      for (const row of plausible) {
        if (near.length >= Number(req.listed)) break
        if (!near.some((n) => n.row.mpn === row.mpn && n.row.manufacturer === row.manufacturer)) {
          near.push({ family, query: q, row: brief(row) })
        }
      }
      nearTotal += plausible.length
    }
  }
  if (truncated) out.mpnScanTruncated = MPN_SCAN_CAP
  if (near.length) Object.assign(out, { match: 'substring', near, nearTotal })
  if (value) {
    const v = await byValue(value, pkg, req)
    if (v) {
      out.byValue = v
      if (!out.match && v.matched) out.match = 'value-package'
    }
  }
  if (!out.match) {
    out.match = 'none'
    const bits = []
    if (cands.length) bits.push(`no catalogue part carries ${cands.map((q) => `'${q}'`).join(' or ')}, in any of ${searchOrder.length} families`)
    if (value && !VALUE_FIELD[value.kind]) bits.push('its value names no family to search')
    else if (value && !pkg) bits.push(`its value ${value.raw} was read, but no package — a value alone does not identify a part`)
    else if (value) bits.push(`no ${VALUE_FIELD[value.kind].family} of ${value.raw} fits package ${pkg.code}` +
                              (out.byValue?.unknownCase ? ` (${out.byValue.unknownCase} of that value state no size)` : ''))
    out.why = bits.join('; ')
  }
  return out
}

async function bom(req) {
  if (!(Number(req.maxResults) > 0) || !(Number(req.listed) > 0)) {
    throw new Error('bom needs positive maxResults and listed')
  }
  const families = allFamilies()
  // Open every shard up front. A family that will not load is an error, not a miss: the
  // alternative is a BOM on which every capacitor reads as absent from the catalogue.
  const broken = []
  for (const f of families) {
    try { ensureShard(f) } catch (e) { broken.push(String(e.message || e)) }
  }
  if (broken.length) {
    throw new Error(`the catalogue could not be opened, so its answers would be incomplete: ${broken.join('; ')}`)
  }
  const groups = []
  for (const line of req.lines ?? []) {
    groups.push({ key: line.key, ...(await identifyLine(line, req, families)) })
  }
  return { groups, families }
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
      : req.op === 'bom' ? await bom(req)
      : (() => { throw new Error(`unknown op '${req.op}'`) })()
    reply({ id: req.id ?? null, ok: true, result })
  } catch (e) {
    reply({ id: req.id ?? null, ok: false, error: String(e.message || e),
            choices: e.choices ?? null })
  }
}
