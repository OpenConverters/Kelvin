// Which comparison model web/src/crossref.js puts in force for a magnetic original
// (famFor -> variantFor). Run: node tools/qarlos/test_crossref_variants.mjs
//
// Qarlos 2026-11-09, seed 20889483: the signal common-mode choke ACM-21-161M-T (MAS subtype
// 'commonModeChoke', no inductance, |Z| = 160 ohm at 100 MHz) was refused with "no L value in the
// catalogue": the inductor model asked it for an inductance its record has no reason to state.
// Row values below are the browse row of the real TAS record.
import assert from 'node:assert/strict'
import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const { famFor, runCrossRef } = await import(
  pathToFileURL(join(here, '..', '..', 'web', 'src', 'crossref.js')).href)

const acm = { mpn: 'ACM-21-161M-T', manufacturer: 'Abracon', device_type: 'commonModeChoke',
  inductance: null, impedance_100mhz: 160, impedance_peak: 160, impedance_peak_freq: 1e8,
  dcr: 0.35, rated_current: 0.33, widthM: 0.0012, heightM: 0.0014 }

// 1. A signal CMC (no L, a 100 MHz |Z|) is ranked on its impedance, within its own device type.
const f = famFor('magnetic', acm)
assert.equal(f.category, 'chipBead', 'signal CMC must take the impedance model')
assert.equal(f.primary, null)
assert.deepEqual(f.sameFacet, { f: 'device_type', label: 'device type' })
assert.equal(f.spec(acm).impedance_100mhz, 160)

// 2. It runs: the pool is restricted to CMCs and the ranker receives the impedance spec.
let seen = null
const engine = {
  async browse(family, q) {
    seen = q
    return { total: 1, rows: [{ ...acm, mpn: 'OTHER-CMC', manufacturer: 'TDK', impedance_100mhz: 150 }] }
  },
  async crossReference(category, orig, cands) {
    assert.equal(category, 'chipBead')
    assert.equal(orig.impedance_100mhz, 160)
    assert.equal(cands[0].impedance_100mhz, 150)
    return { candidates: [] }
  },
}
await runCrossRef({ family: 'magnetic', original: acm, manufacturers: ['TDK'], engine })
assert.deepEqual(seen.filters.device_type, ['commonModeChoke'])

// 3. A power CMC states its common-mode inductance and stays on the inductor model.
const power = { ...acm, mpn: 'POWER-CMC', inductance: 1e-3 }
assert.equal(famFor('magnetic', power).category, 'magnetic')
// 4. A CMC stating neither stays on the inductor model and is refused for the missing L.
const bare = { ...acm, mpn: 'BARE-CMC', impedance_100mhz: null }
assert.equal(famFor('magnetic', bare).category, 'magnetic')
await assert.rejects(runCrossRef({ family: 'magnetic', original: bare, manufacturers: ['TDK'], engine }),
  /no L value/)
// 5. Beads and inductors are unchanged.
assert.equal(famFor('magnetic', { device_type: 'chipBead' }).category, 'chipBead')
assert.equal(famFor('magnetic', { device_type: 'inductor', inductance: 1e-5 }).category, 'magnetic')

// 5b. A cable core (MAS subtype 'cableCore', no inductance; 597 records) takes the impedance
//     model within its own device type (owner 2026-11-14, graph inventory item 11): the
//     inductor model refused every one for "no L value". Row values from the real TAS records:
//     Würth 74271338 (|Z| curve of 429 samples, 140 ohm at 101 MHz), Würth 782114096080 (curve
//     1-7.4 MHz only, no 100 MHz value), Kitagawa RFC-8MA (one sample, 20 ohm at 10 MHz).
const core = { mpn: '74271338', manufacturer: 'Würth Elektronik', device_type: 'cableCore',
  inductance: null, impedance_100mhz: 140, impedance_curve: { x: [1e6, 1e8], y: [10, 140] },
  cable_max_diameter: 0.0158, number_turns: 1 }
const cf = famFor('magnetic', core)
assert.equal(cf.category, 'chipBead', 'cable core must take the impedance model')
assert.deepEqual(cf.sameFacet, { f: 'device_type', label: 'device type' })
assert.equal(cf.spec(core).device_type, 'cableCore')
// Its own axes (owner 2026-11-14): the cable opening (74271338 maximumCableOuterDiameter
// 15.8 mm) and the pass count (numberTurns 1) reach the ranker; not stated -> null.
assert.equal(cf.spec(core).cable_max_diameter_m, 0.0158)
assert.equal(cf.spec(core).number_turns, 1)
assert.equal(cf.spec({ ...core, cable_max_diameter: null }).cable_max_diameter_m, null)
assert.ok(cf.params.some((p) => p.key === 'cable_max_diameter_m'))
assert.ok(cf.params.some((p) => p.key === 'number_turns'))
// A bead's spec does not grow the cable axes.
assert.equal(famFor('magnetic', { device_type: 'chipBead' }).spec(core).number_turns, undefined)
let coreSeen = null
await runCrossRef({ family: 'magnetic', original: core, manufacturers: ['Fair-Rite'], engine: {
  async browse(family, q) { coreSeen = q; return { total: 0, rows: [] } },
  async crossReference() { return { candidates: [] } } } })
assert.deepEqual(coreSeen.filters.device_type, ['cableCore'])
// A curve below 100 MHz only (no 100 MHz value) still takes it: the curve is compared.
assert.equal(famFor('magnetic', { ...core, mpn: '782114096080', impedance_100mhz: null,
  impedance_curve: { x: [1e6, 7.4e6], y: [20, 55.2] } }).category, 'chipBead')
// No |Z| on record (one 10 MHz sample: no 100 MHz value, no curve): inductor model, refused.
const bareCore = { ...core, mpn: 'RFC-8MA', manufacturer: 'Kitagawa', impedance_100mhz: null,
  impedance_curve: null }
assert.equal(famFor('magnetic', bareCore).category, 'magnetic')
await assert.rejects(runCrossRef({ family: 'magnetic', original: bareCore, manufacturers: ['TDK'],
  engine }), /no L value/)

// 6. A bead's operating range reaches the ranker (Qarlos r9 seed 23088792: MFBA3V1608K-471-R,
//    -55..150 degC, took a 125 degC MMZ1608 as 'upgrade' with no temperature verdict).
const bead = famFor('magnetic', { device_type: 'chipBead' })
const beadSpec = bead.spec({ mpn: 'MFBA3V1608K-471-R', device_type: 'chipBead', impedance_100mhz: 470,
  temp_min_c: -55, temp_max_c: 150 })
assert.equal(beadSpec.temp_min_C, -55)
assert.equal(beadSpec.temp_max_C, 150)

// 7. An active oscillator is not asked for a crystal load capacitance (Qarlos r8 seed 22460418:
//    the TCXO ATXAIG-H13-F-25.000MHZ-F25-T3 came back unverified for "missing CL"). Row values
//    are the browse row of the real TAS record.
const tcxo = { mpn: 'ATXAIG-H13-F-25.000MHZ-F25-T3', manufacturer: 'Abracon', device_type: 'oscillator',
  technology: 'tcxo', frequency: 25e6, load_capacitance: null, output_type: 'cmos',
  enable_function: 'outputEnable', vsupply_min: 1.68, vsupply_max: 1.68, frequency_stability: 2.5e-6,
  temp_min_c: -40, temp_max_c: 85, caseCode: '2016 4-SMD' }
let opts = null
const tEngine = {
  async browse() { return { total: 1, rows: [{ ...tcxo, mpn: 'OTHER-TCXO', manufacturer: 'ECS Inc.' }] } },
  async crossReference(category, orig, cands, o) { opts = o; return { candidates: [] } },
}
const tr = await runCrossRef({ family: 'timing', original: tcxo, manufacturers: ['ECS Inc.'], engine: tEngine })
assert.deepEqual(tr.missing, [])
assert.equal(tr.origVerified, true)
assert.equal(opts.original_verified, true)
// A crystal (technology quartzCrystal, typed 'oscillator' in its record like 7V-25.000MAAB-T)
// still needs its CL: without one it is unverified.
const xtal = { ...tcxo, mpn: '7V-25.000MAAB-T', technology: 'quartzCrystal' }
const xr = await runCrossRef({ family: 'timing', original: xtal, manufacturers: ['ECS Inc.'], engine: tEngine })
assert.deepEqual(xr.missing, ['load_capacitance'])
assert.equal(xr.origVerified, false)
// The classification mirrors the ranker's is_passive_resonator.
const { isPassiveResonator } = await import(
  pathToFileURL(join(here, '..', '..', 'web', 'src', 'crossref.js')).href)
for (const [t, d, want] of [['tcxo', 'oscillator', false], ['quartzCrystal', 'oscillator', true],
  ['ceramicResonator', '', true], ['mems', 'crystal', false], ['', 'crystal', true],
  ['', 'oscillator', false], ['xo', 'oscillator', false]]) {
  assert.equal(isPassiveResonator(t, d), want, `${t}/${d}`)
}

// 8. Timing technology crossings (owner decisions 2026-11-13). The pool pre-gate admits exactly
//    the pairs the ranker's timing_technology_cross allows, or the ranker never sees them. Rows
//    are the browse rows of the real TAS 9cb6cf8 records.
const { timingTechnologyCross, timingTechnologiesAdmitted, TIMING_TECHNOLOGIES } = await import(
  pathToFileURL(join(here, '..', '..', 'web', 'src', 'crossref.js')).href)
// The same table as test_crossref_classes.cpp "timing technology crossings".
for (const [o, s, want] of [['mems', 'mems', 'same'], ['mems', 'crystalOscillator', 'memsQuartz'],
  ['crystalOscillator', 'mems', 'memsQuartz'], ['crystalOscillator', 'tcxo', 'tcxoUpgrade'],
  ['tcxo', 'crystalOscillator', null], ['mems', 'tcxo', null], ['tcxo', 'mems', null],
  ['vcxo', 'crystalOscillator', null], ['crystalOscillator', 'vcxo', null],
  ['crystalOscillator', 'ocxo', null], ['crystalOscillator', 'programmable', null],
  ['quartzCrystal', 'crystalOscillator', null], ['ceramicResonator', 'quartzCrystal', null],
  ['mems', 'siliconRC', null], ['', 'mems', null], ['bipolar555', 'cmos555', null]]) {
  assert.equal(timingTechnologyCross(o, s), want, `${o} -> ${s}`)
}
// TIMING_TECHNOLOGIES is the TDAS oscillatorTechnology enum (TDAS schemas/utils.json).
assert.deepEqual(TIMING_TECHNOLOGIES, ['quartzCrystal', 'ceramicResonator', 'crystalOscillator',
  'tcxo', 'vcxo', 'ocxo', 'mems', 'siliconRC', 'programmable'])
assert.deepEqual(timingTechnologiesAdmitted('mems'), ['mems', 'crystalOscillator'])
assert.deepEqual(timingTechnologiesAdmitted('crystalOscillator'), ['crystalOscillator', 'tcxo', 'mems'])
assert.deepEqual(timingTechnologiesAdmitted('tcxo'), ['tcxo'])
assert.deepEqual(timingTechnologiesAdmitted('vcxo'), ['vcxo'])
assert.deepEqual(timingTechnologiesAdmitted('bipolar555'), ['bipolar555'])

const osc = (row) => ({ built_in_capacitance: null, esr: null, frequency_tolerance: null,
  heightM: null, lengthM: null, widthM: null, load_capacitance: null, mode: '', is_production: true,
  rms_phase_jitter: null, ...row })
const asdm1 = osc({ mpn: 'ASDM1-10.000MHZ-LC-T', manufacturer: 'Abracon', device_type: 'oscillator',
  technology: 'mems', frequency: 10e6, frequency_stability: 0.000049999999999999996,
  output_type: 'cmos', enable_function: 'standby', caseCode: '2520 4-SMD', vsupply_min: 3.3,
  vsupply_max: 3.3, temp_min_c: -40, temp_max_c: 85 })
const msh = osc({ mpn: 'MSH302548AH-50.000MHZ-T3', manufacturer: 'MMD', device_type: 'oscillator',
  technology: 'crystalOscillator', frequency: 50e6, frequency_stability: 0.000024999999999999998,
  output_type: 'cmos', enable_function: 'outputEnable', caseCode: '3225 4-SMD', vsupply_min: 3.3,
  vsupply_max: 3.3, temp_min_c: -40, temp_max_c: 85 })
const atx = osc({ ...msh, mpn: 'ATX-H11-E-50.000MHZ-F25', manufacturer: 'Abracon', technology: 'tcxo',
  frequency_stability: 0.0000024999999999999998 })
const ablnoV = osc({ mpn: 'ABLNO-V-155.520MHZ-T2', manufacturer: 'Abracon', device_type: 'oscillator',
  technology: 'vcxo', frequency: 155520000, frequency_stability: 0.000018, output_type: 'cmos',
  enable_function: 'none', caseCode: '14x9 4-SMD', vsupply_min: 3.3, vsupply_max: 3.3,
  rms_phase_jitter: 1e-13, temp_min_c: -40, temp_max_c: 85 })
let tq = null
let ranked = 0
const timingEngine = {
  async browse(family, q) { tq = q; return { total: 1, rows: [{ ...msh, mpn: 'OTHER', manufacturer: 'X' }] } },
  async crossReference() { ranked++; return { candidates: [] } },
}
const pool = async (original) => {
  await runCrossRef({ family: 'timing', original, manufacturers: ['X'], engine: timingEngine })
  return tq.filters.technology
}
assert.deepEqual(await pool(asdm1), ['mems', 'crystalOscillator'])       // MEMS original -> quartz XO
assert.deepEqual(await pool(msh), ['crystalOscillator', 'tcxo', 'mems']) // XO -> TCXO and MEMS
assert.deepEqual(await pool(atx), ['tcxo'])                              // no XO for a TCXO
assert.deepEqual(await pool(ablnoV), ['vcxo'])                           // VCXO stays strict
// "Same technology only" off: no technology filter at all, as before.
await runCrossRef({ family: 'timing', original: asdm1, manufacturers: ['X'], sameType: false, engine: timingEngine })
assert.equal(tq.filters.technology, undefined)

// 9. Frequency is the primary value of an oscillator: AMPDEFH-A02T (Abracon, MEMS, no frequency on
//    record) is refused, worded as every family's missing-primary refusal.
const ampdefh = osc({ mpn: 'AMPDEFH-A02T', manufacturer: 'Abracon', device_type: 'oscillator',
  technology: 'mems', frequency: null, frequency_stability: 0.000024999999999999998,
  output_type: 'cmos', enable_function: 'none', caseCode: '3225 4-SMD', vsupply_min: 1.71,
  vsupply_max: 3.63, temp_min_c: -40, temp_max_c: 85 })
ranked = 0
await assert.rejects(runCrossRef({ family: 'timing', original: ampdefh, manufacturers: ['X'], engine: timingEngine }),
  { message: 'AMPDEFH-A02T has no frequency value in the catalogue — cannot cross-reference without the primary value.' })
assert.equal(ranked, 0)
// A 555 timer has no frequency by nature: NE555 (Texas Instruments, bipolar555) still runs, on
// its own technology only.
const ne555 = osc({ mpn: 'NE555', manufacturer: 'Texas Instruments', device_type: 'timer',
  technology: 'bipolar555', frequency: null, frequency_stability: null, output_type: '',
  enable_function: '', caseCode: 'PDIP', vsupply_min: 4.5, vsupply_max: 16, temp_min_c: 0, temp_max_c: 70 })
assert.deepEqual(await pool(ne555), ['bipolar555'])
// 10. Programmable oscillators (owner 2026-11-13, K2). SiT3901 as TAS 9cb6cf8 has it states
//     neither a frequency nor a frequencyRange: refused like every no-frequency original (this
//     replaced the old blanket exemption for programmable parts). frequency_min / frequency_max
//     are the v22 shard row fields (TDAS electrical.frequencyRange); no TAS record states one
//     yet, so the tests add it.
const sit3901 = osc({ mpn: 'SiT3901', manufacturer: 'SiTime', device_type: 'oscillator',
  technology: 'programmable', frequency: null, frequency_stability: 0.000049999999999999996,
  output_type: 'cmos', enable_function: '', caseCode: '1508, 4 pad', vsupply_min: 1.8,
  vsupply_max: 2.25, rms_phase_jitter: 8e-10, temp_min_c: -40, temp_max_c: 85,
  frequency_min: null, frequency_max: null })
ranked = 0
await assert.rejects(runCrossRef({ family: 'timing', original: sit3901, manufacturers: ['X'], engine: timingEngine }),
  { message: 'SiT3901 has no frequency value in the catalogue — cannot cross-reference without the primary value.' })
assert.equal(ranked, 0)
// A range alone is not enough: both ends are needed.
await assert.rejects(runCrossRef({ family: 'timing', original: { ...sit3901, frequency_min: 1e6 },
  manufacturers: ['X'], engine: timingEngine }), /has no frequency value/)

// An engine that records EVERY browse, so the second (blank) pool is visible.
let queries = []
const blankRows = [
  { ...sit3901, mpn: 'BLANK-1-220', manufacturer: 'X', frequency_min: 1e6, frequency_max: 220e6 },
  { ...sit3901, mpn: 'ORDERED-12', manufacturer: 'X', frequency: 12e6, frequency_min: 1e6, frequency_max: 220e6 },
]
let rankedPool = null
const recEngine = {
  async browse(family, q) { queries.push(q); return q.filters.frequency_min ? { total: blankRows.length, rows: blankRows } : { total: 1, rows: [{ ...msh, mpn: 'EXACT', manufacturer: 'X' }] } },
  async crossReference(cat, orig, cands) { rankedPool = cands.map((c) => c.mpn); return { candidates: [] } },
}
// The range-only original: one pool, programmable only, the candidate's range must cover
// 1-26 MHz (widened by the 1e-9 representation epsilon), and no frequency filter.
const sitRange = { ...sit3901, frequency_min: 1e6, frequency_max: 26e6 }
queries = []
const rr = await runCrossRef({ family: 'timing', original: sitRange, manufacturers: ['X'], engine: recEngine })
assert.equal(queries.length, 1)
assert.deepEqual(queries[0].filters.technology, ['programmable'])
assert.equal(queries[0].filters.frequency, undefined)
assert.ok(queries[0].filters.frequency_min.max >= 1e6 && queries[0].filters.frequency_min.max < 1e6 * (1 + 1e-8))
assert.ok(queries[0].filters.frequency_max.min <= 26e6 && queries[0].filters.frequency_max.min > 26e6 * (1 - 1e-8))
assert.equal(rr.origSpec.frequency_range_min, 1e6)
assert.equal(rr.origSpec.frequency_range_max, 26e6)
// Its range is its datum: the original is verified on it, not marked missing a frequency.
assert.deepEqual(rr.missing, [])
// A programmable original ordered at 10 MHz: the exact-frequency pool AND a second pool of
// programmable blanks whose range holds 10 MHz; the ordered part from the second pool is
// dropped (the ranker rejects any stated frequency but 10 MHz), the blank is ranked.
const sitAt10 = { ...sitRange, frequency: 10e6 }
queries = []
await runCrossRef({ family: 'timing', original: sitAt10, manufacturers: ['X'], engine: recEngine })
assert.equal(queries.length, 2)
assert.ok(queries[0].filters.frequency.min <= 10e6 && queries[0].filters.frequency.max >= 10e6)
assert.equal(queries[0].filters.frequency_min, undefined)
assert.equal(queries[1].filters.frequency, undefined)
assert.deepEqual(queries[1].filters.technology, ['programmable'])
assert.ok(queries[1].filters.frequency_min.max >= 10e6 && queries[1].filters.frequency_max.min <= 10e6)
assert.deepEqual(rankedPool, ['EXACT', 'BLANK-1-220'])
// Fixed-frequency oscillators are untouched: one pool, no range filters, the crossings as before.
for (const o of [msh, asdm1, atx]) {
  queries = []
  await runCrossRef({ family: 'timing', original: o, manufacturers: ['X'], engine: recEngine })
  assert.equal(queries.length, 1, o.mpn)
  assert.equal(queries[0].filters.frequency_min, undefined, o.mpn)
  assert.equal(queries[0].filters.frequency_max, undefined, o.mpn)
}
// Same technology only: a programmable original's pool is programmable alone, and no other
// technology's pool admits programmable (timing_technology_cross has no crossing for it).
assert.deepEqual(timingTechnologiesAdmitted('programmable'), ['programmable'])
for (const t of TIMING_TECHNOLOGIES) {
  if (t !== 'programmable') assert.ok(!timingTechnologiesAdmitted(t).includes('programmable'), t)
}

console.log('crossref variants: all checks passed')
