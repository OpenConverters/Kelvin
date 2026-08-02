// The cross-reference model, shared by the UI and by anything else that must run
// the REAL cross-reference (Qarlos, the hourly auditor, drives this exact path).
//
// It used to live inside CrossRefView.vue, which meant the only way to exercise the
// production pipeline was to drive the browser. A second copy of this table would
// drift from it within a release, and an auditor comparing against a drifted copy
// audits a fiction — so there is one table, imported by both.
//
// Each entry mirrors CrossRef.hpp exactly: `spec` builds the flat spec block the
// ranker reads, `hardKeys` are the hard gates (reject edges), `accept`/`hardMin`
// pre-filter candidates at the ranker's OWN reject boundaries — the pre-gate never
// drops a part the ranker would have kept.
// The default engine is the browser one (WASM in a worker, shards over HTTP).
// It is INJECTABLE so a headless caller can supply the same WASM module loaded in
// Node with shards read off disk — same binary, same ranking, different transport.
// This seam is the whole reason the auditor can check the real pipeline instead of
// a re-implementation of it.
import * as browserEngine from './engine.js'

// Families the C++ ranker has a parameter model for (gates_for/primary_value_spec).
// Each entry mirrors CrossRef.hpp exactly: `spec` builds the flat spec block the
// ranker reads, `hardKeys` are the hard gates (reject edges), `accept`/`hardMin`
// pre-filters candidates at the ranker's OWN reject boundaries — the pre-gate
// never drops a part the ranker would have kept.
export const XREF = [
  {
    key: 'magnetic', label: 'Magnetics', category: 'magnetic',
    primary: { row: 'inductance', label: 'L', unit: 'H', acceptLo: 0.80, acceptHi: 1.25 },
    sameFacet: { f: 'device_type', label: 'device type' },
    hardKeys: ['saturation_current', 'rated_current'],
    // A ferrite bead lives in the magnetic catalogue but is not an inductor: it
    // has no inductance value at all, and is characterised by its impedance
    // CURVE. Ranking it against the inductor config would ask for a primary
    // value that does not exist, so the family adapts to the chosen original.
    variantFor: (orig) => (orig?.device_type === 'chipBead' ? BEAD_VARIANT : null),
    spec: (r) => ({ ...base(r), value_si: nz(r.inductance), saturation_current: nz(r.saturation_current), rated_current: nz(r.rated_current), dcr: nz(r.dcr) }),
    params: [
      { key: 'value', label: 'L', row: 'inductance', unit: 'H' },
      { key: 'saturation_current', label: 'Isat', row: 'saturation_current', unit: 'A' },
      { key: 'rated_current', label: 'I rated', row: 'rated_current', unit: 'A' },
      { key: 'dcr', label: 'DCR', row: 'dcr', unit: 'Ω' },
    ],
  },
  {
    key: 'capacitor', label: 'Capacitors', category: 'capacitor',
    primary: { row: 'capacitance', label: 'C', unit: 'F', acceptLo: 0.80, acceptHi: 4.00 },
    sameFacet: { f: 'technology', label: 'technology' },
    hardKeys: ['voltage'],
    hardMin: [{ row: 'v_rated', factor: 0.9 }],
    spec: (r) => ({ ...base(r), value_si: nz(r.capacitance), voltage: nz(r.v_rated), esr: nz(r.esr), ripple_current: nz(r.ripple_current_rms), technology: r.technology ?? '', dielectric_code: r.dielectric_code ?? '', esr_frequency: nz(r.esr_frequency), temp_min_C: r.temp_min_c, temp_max_C: r.temp_max_c }),
    params: [
      { key: 'value', label: 'C', row: 'capacitance', unit: 'F' },
      { key: 'voltage', label: 'V rated', row: 'v_rated', unit: 'V' },
      { key: 'esr', label: 'ESR', row: 'esr', unit: 'Ω' },
      { key: 'ripple_current', label: 'I ripple', row: 'ripple_current_rms', unit: 'A' },
    ],
  },
  {
    key: 'resistor', label: 'Resistors', category: 'resistor',
    primary: { row: 'resistance', label: 'R', unit: 'Ω', acceptLo: 0.95, acceptHi: 1.05 },
    sameFacet: null,
    hardKeys: [],
    // `family` is the device class: a chip resistor ARRAY ("Chip Resistor Array",
    // YAGEO YC/TC) is N isolated elements on 2N pads inside an ordinary chip
    // outline, so without it the ranker judged a 4-element array by body size
    // alone and called discrete 1206 chips drop-ins for it (ABT #481).
    spec: (r) => ({ ...base(r), value_si: nz(r.resistance), power_rating: nz(r.power_rating), tolerance_pct: r.tolerance != null && r.tolerance > 0 ? r.tolerance * 100 : null, family: r.family ?? '' }),
    params: [
      { key: 'value', label: 'R', row: 'resistance', unit: 'Ω' },
      { key: 'power_rating', label: 'P', row: 'power_rating', unit: 'W' },
      { key: 'tolerance_pct', label: 'tol', row: 'tolerance', unit: '%', scale: 100 },
    ],
  },
  {
    key: 'mosfet', label: 'MOSFETs', category: 'mosfet',
    primary: null,
    sameFacet: { f: 'technology', label: 'technology' },
    hardKeys: ['vds'],
    hardMin: [{ row: 'vds_rated', factor: 0.9 }],
    spec: (r) => ({ ...base(r), vds: nz(r.vds_rated), id: nz(r.id_continuous), rds_on: nz(r.rds_on), qg: nz(r.qg_total), coss: nz(r.coss), vgs_threshold_max: nz(r.vgs_threshold_max), rds_on_vgs: nz(r.rds_on_vgs), vgs_max: nz(r.vgs_max), technology: r.technology ?? '', qualification: r.qualification ?? '' }),
    params: [
      { key: 'vds', label: 'Vds', row: 'vds_rated', unit: 'V' },
      { key: 'id', label: 'Id', row: 'id_continuous', unit: 'A' },
      { key: 'rds_on', label: 'Rds(on)', row: 'rds_on', unit: 'Ω' },
      { key: 'qg', label: 'Qg', row: 'qg_total', unit: 'C' },
    ],
  },
  {
    key: 'timing', label: 'Crystals', category: 'timeBase',
    // Frequency is the identity here, not a value to rank near — a 16 MHz part
    // is not a near-match for 12 MHz. The load-capacitance gate is the one that
    // matters most: a crystal is specified AT a CL, and the board's load network
    // is built for it.
    primary: null,
    sameFacet: { f: 'technology', label: 'technology' },
    hardKeys: ['frequency', 'technology', 'load_capacitance'],
    exactNum: [{ row: 'frequency', tol: 1e-4 }],
    spec: (r) => ({ ...base(r), subtype: r.device_type ?? '', technology: r.technology ?? '',
      device_type: r.device_type ?? '', frequency: nz(r.frequency),
      load_capacitance: nz(r.load_capacitance),
      load_capacitance_pF: r.load_capacitance != null ? r.load_capacitance * 1e12 : null,
      esr: nz(r.esr), output_type: r.output_type ?? '', mode: r.mode ?? '',
      tolerance_ppm: r.frequency_tolerance != null ? r.frequency_tolerance * 1e6 : null,
      stability_ppm: r.frequency_stability != null ? r.frequency_stability * 1e6 : null }),
    params: [
      { key: 'frequency', label: 'f', row: 'frequency', unit: 'Hz' },
      { key: 'load_capacitance_pF', label: 'CL', row: 'load_capacitance', unit: 'F' },
      { key: 'esr', label: 'ESR', row: 'esr', unit: 'Ω' },
      { key: 'tolerance_ppm', label: 'tol', row: 'frequency_tolerance', unit: 'ppm', scale: 1e6 },
    ],
  },
  {
    key: 'connector', label: 'Connectors', category: 'connector',
    // Identity is family + position count; a 6-way terminal block is not a
    // 4-way one. Pitch is the land pattern — a connector record carries no body
    // outline, so it is the only footprint datum there is, and it must reach the
    // ranker: it was dropped here and a 2.54 mm part was graded a drop_in for a
    // 2.00 mm original (ABT #485). Contact plating, termination and mating cycles are
    // the rest of "does it mate", and the caveat used to declare all three absent from
    // the catalogue while 97,144 / 9,908 / 56,573 records stated them — the sentence
    // described what this file extracted, not what the records hold, so it told the
    // engineer a checkable gate was uncheckable (ABT #487).
    // The operating-temperature range is the other hard limit a connector record
    // states, and it was dropped the same way: a +105 degC substitute for a +125 degC
    // original came back "upgrade" on rated current alone, with no temperature row in
    // the verdict table at all (ABT #520).
    primary: null,
    sameFacet: { f: 'family', label: 'family' },
    hardKeys: ['family', 'positions', 'rated_current_A'],
    exactNum: [{ row: 'positions', tol: 1e-9 }],
    // What remains genuinely unchecked is the COUNTERPART, not the catalogue: 193,951
    // records name the series they mate with, but those lists are written per vendor and
    // no second source ever appears in one, so they cannot settle a cross-vendor swap.
    caveat: 'Contact pitch, plating, termination and mating-cycle rating are compared where both records state one; the counterpart itself is NOT checked — no candidate is verified against the mating half already on your board. Confirm the pair mates before substituting.',
    // pitch_mm, not metres: the ranker's ParamSpec is keyed on it and an engineer
    // reads a connector pitch in millimetres. The shard stores SI metres. The
    // temperatures pass through raw, NOT through nz(): a cold limit is negative and
    // 0 degC is a real rating, so "positive" would discard both. The two mating strings
    // fall back to null, NOT '': the ranker's ExactMatch counts '' as a stated value, so
    // an original with no plating on record would FAIL every candidate that states one —
    // an unknown reported as a mismatch.
    spec: (r) => ({ ...base(r), family: r.family ?? '', positions: nz(r.positions),
      polarity: r.polarity ?? '', interface_standard: r.interface_standard ?? '',
      pitch_mm: r.pitch != null && r.pitch > 0 ? r.pitch * 1e3 : null,
      rated_current_A: nz(r.rated_current), rated_voltage_V: nz(r.rated_voltage),
      temp_min_C: r.temp_min_c, temp_max_C: r.temp_max_c,
      contact_plating: r.contact_plating || null, termination: r.termination || null,
      mating_cycles: nz(r.mating_cycles) }),
    params: [
      { key: 'positions', label: 'pos', row: 'positions', unit: '' },
      { key: 'pitch_mm', label: 'pitch', row: 'pitch', unit: 'm' },
      { key: 'rated_current_A', label: 'I/contact', row: 'rated_current', unit: 'A' },
      { key: 'rated_voltage_V', label: 'V rated', row: 'rated_voltage', unit: 'V' },
      { key: 'temp_max_C', label: 'T max', row: 'temp_max_c', unit: '°C' },
      { key: 'contact_plating', label: 'plating', row: 'contact_plating', str: true },
    ],
  },
  {
    key: 'diode', label: 'Diodes', category: 'diode',
    // vrrm compares the MARKED breakdown voltage. For a zener that is half the spec:
    // the A grade and the B grade of a BZX84-A3V6 are both marked 3.6 V and guarantee
    // 3.56-3.64 V and 3.42-3.78 V respectively. The band was dropped here, so every
    // candidate scored "vrrm: pass" on the nominal and nothing in the result said the
    // grade was wider — or, for a record that states none, unknown (ABT #488).
    primary: null,
    sameFacet: { f: 'technology', label: 'type' },
    hardKeys: ['vrrm'],
    hardMin: [{ row: 'vrrm_rated', factor: 0.9 }],
    // The bounds travel in volts (the note quotes them) and the grade as a percentage,
    // which is what the ranker's ParamSpec compares — the shard stores it as a fraction,
    // the way the resistor tolerance is stored. NOT through nz(): a 0 % band would be a
    // stated value, not an absence, and 'positive' would silently discard it.
    spec: (r) => ({ ...base(r), vrrm: nz(r.vrrm_rated), if_avg: nz(r.if_avg_rated), vf: nz(r.vf_typ), qrr: nz(r.qrr), trr: nz(r.trr), technology: r.technology ?? '',
      vz_min_V: nz(r.vz_min), vz_max_V: nz(r.vz_max),
      vz_tolerance_pct: r.vz_tolerance != null ? r.vz_tolerance * 100 : null }),
    params: [
      { key: 'vrrm', label: 'Vrrm', row: 'vrrm_rated', unit: 'V' },
      { key: 'if_avg', label: 'If avg', row: 'if_avg_rated', unit: 'A' },
      { key: 'vf', label: 'Vf', row: 'vf_typ', unit: 'V' },
      { key: 'qrr', label: 'Qrr', row: 'qrr', unit: 'C' },
      { key: 'vz_tolerance_pct', label: 'Vbr tol', row: 'vz_tolerance', unit: '%', scale: 100 },
    ],
  },
]

// Ferrite-bead view of the magnetic catalogue. Its primary axis is impedance at
// 100 MHz — the number the whole industry quotes — but the ranker also compares
// the peak of the measured curve and the frequency it occurs at, because parts
// with identical Z@100MHz routinely peak in completely different bands.
const BEAD_VARIANT = {
  category: 'chipBead',
  primary: null,
  sameFacet: { f: 'device_type', label: 'device type' },
  hardKeys: [],
  spec: (r) => ({ ...base(r), impedance_100mhz: nz(r.impedance_100mhz),
    impedance_peak: nz(r.impedance_peak), impedance_peak_freq: nz(r.impedance_peak_freq),
    srf: nz(r.srf), dcr: nz(r.dcr), rated_current: nz(r.rated_current) }),
  params: [
    { key: 'impedance_100mhz', label: 'Z@100M', row: 'impedance_100mhz', unit: 'Ω' },
    { key: 'impedance_peak', label: 'peak Z', row: 'impedance_peak', unit: 'Ω' },
    { key: 'impedance_peak_freq', label: 'peak @', row: 'impedance_peak_freq', unit: 'Hz' },
    { key: 'dcr', label: 'DCR', row: 'dcr', unit: 'Ω' },
    { key: 'rated_current', label: 'I rated', row: 'rated_current', unit: 'A' },
  ],
}

// Shard rows carry 0 (fixed fields) or null (nullable fields) for "not in the
// datasheet" — both mean UNKNOWN to the ranker, never a real datum.
export function nz(v) { return v != null && v > 0 ? v : null }

// Fields every category contributes regardless of its electrical parameters:
// identity (drives the AEC-Q / voltage MPN decode), physical size and case code
// (the footprint + mount-type gates), and lifecycle. Dimensions are metres, as
// the shard stores them; the engine falls back to resolving the case code when a
// record has no mechanical drawing.
export function base(r) {
  return {
    mpn: r.mpn,
    length_m: nz(r.lengthM),
    width_m: nz(r.widthM),
    height_m: nz(r.heightM),
    case_code: r.caseCode ?? '',
    mount: r.mount ?? '',
    is_production: r.is_production,
  }
}

// Collision-proof row key: two vendors can ship the same MPN string, so the
// caller-side identity is manufacturer+mpn. It travels in `_key`, which the
// engine echoes verbatim — NOT in `mpn` (which must stay the real part number,
// because the AEC-Q and rated-voltage gates decode it) and NOT in `id`, which is
// the MOSFET DRAIN-CURRENT parameter. Using `id` silently overwrote the current
// and disabled that comparison entirely.
const SEP = '␟'
export const keyOf = (r) => `${r.manufacturer}${SEP}${r.mpn}`

// The FAE honesty rule (CrossRef.hpp): with the original's hard-gate specs
// unknown, no candidate may rank as a clean 'recommended'.
export function originalMissing(f, spec) {
  const miss = []
  if (f.primary && spec.value_si == null) miss.push(f.primary.label)
  for (const k of f.hardKeys) {
    if (spec[k] == null) miss.push(f.params.find((p) => p.key === k)?.label ?? k)
  }
  return miss
}

// candidate pool cap: browse can hand back thousands; scoring is cheap but the
// pool is capped and the cap is REPORTED (no silent truncation).
export const POOL_LIMIT = 800

export const xrefByKey = (key) => XREF.find((f) => f.key === key) ?? XREF[0]

// The config actually in force: the family, with any per-original variant applied.
// `fam.key` stays the SHARD family (browse/drawer/pin key on it); only the
// comparison model changes.
export function famFor(familyKey, original) {
  const f = xrefByKey(familyKey)
  const v = f.variantFor?.(original)
  return v ? { ...f, ...v } : f
}

// Run the real cross-reference for one original against one or more target
// manufacturers. Returns exactly what the view renders — no UI state, no refs —
// so a headless caller gets the same verdicts a user sees.
export async function runCrossRef({ family, original, manufacturers, sameType = true,
                                    maxResults = 25, poolLimit = POOL_LIMIT,
                                    engine = browserEngine }) {
  const { browse, crossReference } = engine
  const f = famFor(family, original)
  const origSpec = f.spec(original)
  if (f.primary && origSpec.value_si == null) {
    throw new Error(`${original.mpn} has no ${f.primary.label} value in the catalogue — cannot cross-reference without the primary value.`)
  }

  // Pre-gate candidates AT the ranker's own reject edges (accept window /
  // hard-gate floor against a known original) — nothing the ranker would
  // keep is dropped, and the pool stays scoreable.
  const filters = { manufacturer: manufacturers }
  if (f.primary) {
    filters[f.primary.row] = { min: origSpec.value_si * f.primary.acceptLo, max: origSpec.value_si * f.primary.acceptHi }
  }
  for (const h of f.hardMin ?? []) {
    const o = nz(original[h.row])
    if (o != null) filters[h.row] = { min: o * h.factor }
  }
  // IDENTITY parameters constrain the pool rather than just failing later: a
  // 25 MHz crystal is not a near-match for a 48 MHz one, so ranking it and
  // rejecting it fills the table with 25 rows of "no substitute" and buries
  // the parts that could actually work.
  for (const e of f.exactNum ?? []) {
    const v = nz(original[e.row])
    if (v != null) filters[e.row] = { min: v * (1 - e.tol), max: v * (1 + e.tol) }
  }
  if (sameType && f.sameFacet && original[f.sameFacet.f]) {
    filters[f.sameFacet.f] = [original[f.sameFacet.f]]
  }

  const pool = await browse(f.key, { filters, sort: { field: 'lineno', dir: 'asc' }, limit: poolLimit })
  const rows = pool.rows.filter((r) => !(r.mpn === original.mpn && r.manufacturer === original.manufacturer))
  const rowByKey = new Map(rows.map((r) => [keyOf(r), r]))
  const missing = originalMissing(f, origSpec)
  const origVerified = missing.length === 0
  const ranked = rows.length
    ? (await crossReference(f.category, { ...origSpec, _key: keyOf(original) },
        rows.map((r) => ({ ...f.spec(r), _key: keyOf(r) })),
        { original_verified: origVerified, max_results: Number(maxResults) })).candidates
    : []
  return { fam: f, origSpec, ranked, rowByKey, poolTotal: pool.total,
           poolScored: rows.length, origVerified, missing }
}
