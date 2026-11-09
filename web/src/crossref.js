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
    // saturation_points is the record's WHOLE saturation table (v20 row field, built by the
    // engine's browse as [{current, percent_drop, temperature}], null criterion = not stated):
    // 10 % / 20 % / 30 % ratings are different numbers, the ranker compares them only at a
    // common criterion (ABT #2042), and an unknown-criterion Isat is held against the other
    // side's 10-30 % span — which needs every point, not the one nearest 20 % (Qarlos
    // 2026-11-08, BWVS00606045150T00 vs MSS6132T-123MLC). No table -> null, and the scalar is
    // compared as a basis-unknown value.
    // srf: the self-resonant frequency, compared like the bead's (Qarlos 2026-11-09).
    spec: (r) => ({ ...base(r), value_si: nz(r.inductance), saturation_current: nz(r.saturation_current),
      saturation_points: Array.isArray(r.saturation_points) && r.saturation_points.length
        ? r.saturation_points : null,
      rated_current: nz(r.rated_current), dcr: nz(r.dcr), srf: nz(r.srf),
      // v20 (Qarlos 2026-11-09): the operating range, which the substitute must cover at both
      // ends. Raw, NOT through nz(): a cold limit is negative and 0 degC is a real rating.
      // Absent -> null -> UNVERIFIED in the ranker.
      temp_min_C: r.temp_min_c ?? null, temp_max_C: r.temp_max_c ?? null }),
    params: [
      { key: 'value', label: 'L', row: 'inductance', unit: 'H' },
      { key: 'saturation_current', label: 'Isat', row: 'saturation_current', unit: 'A' },
      { key: 'rated_current', label: 'I rated', row: 'rated_current', unit: 'A' },
      { key: 'dcr', label: 'DCR', row: 'dcr', unit: 'Ω' },
      { key: 'srf', label: 'SRF', row: 'srf', unit: 'Hz' },
      { key: 'temp_max_C', label: 'T max', row: 'temp_max_c', unit: '°C' },
    ],
  },
  {
    key: 'capacitor', label: 'Capacitors', category: 'capacitor',
    primary: { row: 'capacitance', label: 'C', unit: 'F', acceptLo: 0.80, acceptHi: 4.00 },
    sameFacet: { f: 'technology', label: 'technology' },
    hardKeys: ['voltage'],
    // An AC-only original (line-safety X/Y) has no DC rating to gate on; its AC rating
    // stands in, and only then — a DC original is never gated on an AC figure.
    hardMin: [{ row: 'v_rated', factor: 0.9 }, { row: 'v_rated_ac', factor: 0.9, onlyWithout: 'v_rated' }],
    // `family` is the series name, and for a capacitor across the mains it was the
    // only place the IEC 60384-14 line-safety approval appeared until CAS gained
    // part.safetyClass (carried below as `safety_class`, v20, and read first by the
    // ranker); before that the vendors' own series names ("MKP-X1 R",
    // "MKP-X2", "MKP-Y2", "R47 X1 440 VAC") are the whole evidence base. Without it
    // the ranker compared a 560 V catalogue figure against 440 V and graded six X2
    // parts drop-in upgrades for an X1 original (ABT #557). The ranker reads it ONLY
    // when the ORIGINAL's series names a class, so nothing changes for the rest of
    // the catalogue.
    spec: (r) => ({ ...base(r), value_si: nz(r.capacitance), voltage: nz(r.v_rated), esr: nz(r.esr), ripple_current: nz(r.ripple_current_rms), technology: r.technology ?? '', dielectric_code: r.dielectric_code ?? '', family: r.family ?? '', voltage_ac: nz(r.v_rated_ac), esr_frequency: nz(r.esr_frequency), temp_min_C: r.temp_min_c, temp_max_C: r.temp_max_c,
      // v20 (Qarlos 2026-11-09): the frequency the ripple rating is stated at (a 100 Hz and a
      // 20 kHz rating are compared only on a common basis), and the IEC 60384-14 safety class
      // the record states (CAS part.safetyClass.class). Absent -> null, never a typical value.
      ripple_current_frequency: nz(r.ripple_current_frequency), safety_class: r.safety_class || null }),
    params: [
      { key: 'value', label: 'C', row: 'capacitance', unit: 'F' },
      { key: 'voltage', label: 'V rated', row: 'v_rated', unit: 'V' },
      { key: 'voltage_ac', label: 'V rated AC', row: 'v_rated_ac', unit: 'V' },
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
    // The power rating's temperature basis travels with it: a rating is stated AT an ambient
    // and derates along the record's curve above it, so the ranker compares the power each
    // part delivers at the ORIGINAL's rating temperature. Raw, NOT through nz(): 0 degC would
    // be a real temperature. Absent -> null, and the ranker reports the power UNVERIFIED
    // wherever the comparison needs the missing basis.
    spec: (r) => ({ ...base(r), value_si: nz(r.resistance), power_rating: nz(r.power_rating), tolerance_pct: r.tolerance != null && r.tolerance > 0 ? r.tolerance * 100 : null, family: r.family ?? '',
      power_rating_temperature: r.power_rating_temperature ?? null,
      derating_temperature: r.derating_temperature ?? null, derating_amplitude: r.derating_amplitude ?? null,
      // v19 (Qarlos 2026-11-08): the resistive element and the function the record states
      // (current sensing, pulse/surge withstanding) — a swap across either is not a drop-in;
      // the TCR, compared as a magnitude; and the operating range, which the substitute must
      // cover. Temperatures and TCR raw, NOT through nz(): 0 degC and a negative TCR are
      // stated values. Absent -> null -> UNVERIFIED in the ranker.
      technology: r.technology ?? '', tcr: r.tcr ?? null,
      temp_min_C: r.temp_min_c ?? null, temp_max_C: r.temp_max_c ?? null,
      current_sense: r.current_sense === true, pulse_withstanding: r.pulse_withstanding === true }),
    params: [
      { key: 'value', label: 'R', row: 'resistance', unit: 'Ω' },
      { key: 'power_rating', label: 'P', row: 'power_rating', unit: 'W' },
      { key: 'tolerance_pct', label: 'tol', row: 'tolerance', unit: '%', scale: 100 },
      { key: 'tcr', label: 'TCR', row: 'tcr', unit: 'ppm/K' },
      { key: 'temp_max_C', label: 'T max', row: 'temp_max_c', unit: '°C' },
    ],
  },
  {
    key: 'mosfet', label: 'MOSFETs', category: 'mosfet',
    primary: null,
    sameFacet: { f: 'technology', label: 'technology' },
    hardKeys: ['vds'],
    hardMin: [{ row: 'vds_rated', factor: 0.9 }],
    // A P-channel part never substitutes for an N-channel one: keep the pool to the original's channel.
    sameStr: ['channel'],
    spec: (r) => ({ ...base(r), vds: nz(r.vds_rated), id: nz(r.id_continuous), rds_on: nz(r.rds_on), qg: nz(r.qg_total), coss: nz(r.coss), vgs_threshold_max: nz(r.vgs_threshold_max), rds_on_vgs: nz(r.rds_on_vgs), vgs_max: nz(r.vgs_max), technology: r.technology ?? '', qualification: r.qualification ?? '', channel: r.channel || null }),
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
    // Equal within the representation epsilon (1e-9 = 0.001 ppm), never a band: the old
    // 1e-4 (100 ppm) pool let 25.00125 MHz and 24.9987 MHz parts in for a 25 MHz original,
    // and the ranker graded them 'pass' (Qarlos 2026-11-09). Matches the ranker's kTimeBase.
    exactNum: [{ row: 'frequency', tol: 1e-9 }],
    spec: (r) => ({ ...base(r), subtype: r.device_type ?? '', technology: r.technology ?? '',
      device_type: r.device_type ?? '', frequency: nz(r.frequency),
      load_capacitance: nz(r.load_capacitance),
      load_capacitance_pF: r.load_capacitance != null ? r.load_capacitance * 1e12 : null,
      // output_type and mode are hard identity keys in the ranker. Blank falls back to
      // null, NOT '': the ranker's ExactMatch counts '' as a stated value whenever the
      // other side states one, so an oscillator record with no outputType was REJECTED
      // as "an identity parameter differs" against an LVDS original — an unknown
      // reported as a mismatch, the same trap as the connector strings (ABT #505).
      esr: nz(r.esr), output_type: r.output_type || null, mode: r.mode || null,
      // v20: the control pin's function (none / outputEnable / standby / triState), a hard
      // identity key in the ranker; blank -> null for the same reason as output_type.
      enable_function: r.enable_function || null,
      // The supply range of an active oscillator: the substitute's must cover the
      // original's (the ranker FAILs a shortfall at either end and skips the keys for a
      // passive crystal or resonator, which has no supply). Neither was passed, so a
      // 3.3 V-only and a 1.8 V-only XO ranked minor_review / upgrade for a 2.5 V original.
      supply_min_V: nz(r.vsupply_min), supply_max_V: nz(r.vsupply_max),
      tolerance_ppm: r.frequency_tolerance != null ? r.frequency_tolerance * 1e6 : null,
      stability_ppm: r.frequency_stability != null ? r.frequency_stability * 1e6 : null,
      // Raw, NOT through nz(): a cold limit is negative and 0 degC is a real rating. The
      // ranker's PARAM_SPECS compare both ends (ABT #2033, #2046).
      temp_min_C: r.temp_min_c ?? null, temp_max_C: r.temp_max_c ?? null }),
    params: [
      { key: 'frequency', label: 'f', row: 'frequency', unit: 'Hz' },
      { key: 'load_capacitance_pF', label: 'CL', row: 'load_capacitance', unit: 'F' },
      { key: 'esr', label: 'ESR', row: 'esr', unit: 'Ω' },
      { key: 'tolerance_ppm', label: 'tol', row: 'frequency_tolerance', unit: 'ppm', scale: 1e6 },
      { key: 'supply_min_V', label: 'Vdd min', row: 'vsupply_min', unit: 'V' },
      { key: 'supply_max_V', label: 'Vdd max', row: 'vsupply_max', unit: 'V' },
      { key: 'output_type', label: 'output', row: 'output_type', str: true },
      { key: 'enable_function', label: 'pin 1', row: 'enable_function', str: true },
      { key: 'stability_ppm', label: 'stab', row: 'frequency_stability', unit: 'ppm', scale: 1e6 },
      { key: 'temp_min_C', label: 'T min', row: 'temp_min_c', unit: '°C' },
      { key: 'temp_max_C', label: 'T max', row: 'temp_max_c', unit: '°C' },
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
    caveat: 'Board mount, contact pitch, plating, termination and mating-cycle rating are compared where both records state one; the counterpart itself is NOT checked — no candidate is verified against the mating half already on your board. Confirm the pair mates before substituting.',
    // pitch_mm, not metres: the ranker's ParamSpec is keyed on it and an engineer
    // reads a connector pitch in millimetres. The shard stores SI metres. The
    // temperatures pass through raw, NOT through nz(): a cold limit is negative and
    // 0 degC is a real rating, so "positive" would discard both. EVERY optional identity
    // string falls back to null, NOT '': the ranker's ExactMatch counts '' as a stated
    // value and only treats it as unknown when BOTH sides are blank, so an original whose
    // record states no plating, polarity or interface FAILed every candidate that states
    // one — an unknown reported as a mismatch. polarity and interface_standard were still
    // passing '' when the RF interface started reaching them (ABT #505).
    spec: (r) => ({ ...base(r), family: r.family ?? '', positions: nz(r.positions),
      polarity: r.polarity || null, interface_standard: r.interface_standard || null,
      pitch_mm: r.pitch != null && r.pitch > 0 ? r.pitch * 1e3 : null,
      // mechanical.rows: positions / rows is the row the ranker's pin-field check runs
      // over (a small pitch difference adds up along a row). null when unstated.
      rows: nz(r.rows),
      rated_current_A: nz(r.rated_current), rated_voltage_V: nz(r.rated_voltage),
      temp_min_C: r.temp_min_c, temp_max_C: r.temp_max_c,
      contact_plating: r.contact_plating || null, termination: r.termination || null,
      // The board attach (mechanical.mountingStyle: smt / tht / pressFit / skedd / panel /
      // cable), under the key the ranker's shared mount gate reads for a connector
      // (compare_mount, which classifies press-fit, skedd, panel and cable as board
      // processes of their own). null when unstated: unverified, not refused.
      mounting: r.mount || null,
      // The mating-axis orientation (vertical / rightAngle / horizontal / mezzanine / edge /
      // cable): a vertical FPC connector for a right-angle original is a different
      // mechanical design. null when unstated: unverified with a note, never a match.
      orientation: r.orientation || null,
      // familyDetails.pluggable (terminal blocks): true = pluggable plug, false = fixed PCB
      // block, null = unstated. Tri-state on purpose: unstated is never 'fixed'.
      pluggable: r.pluggable === 'true' ? true : r.pluggable === 'false' ? false : null,
      mating_cycles: nz(r.mating_cycles) }),
    params: [
      { key: 'positions', label: 'pos', row: 'positions', unit: '' },
      { key: 'pitch_mm', label: 'pitch', row: 'pitch', unit: 'm' },
      { key: 'rated_current_A', label: 'I/contact', row: 'rated_current', unit: 'A' },
      { key: 'rated_voltage_V', label: 'V rated', row: 'rated_voltage', unit: 'V' },
      { key: 'temp_max_C', label: 'T max', row: 'temp_max_c', unit: '°C' },
      { key: 'contact_plating', label: 'plating', row: 'contact_plating', str: true },
      { key: 'mounting', label: 'mount', row: 'mount', str: true },
      { key: 'orientation', label: 'orientation', row: 'orientation', str: true },
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
    // Recovery is the same case, now that the shard tells the two apart (ABT #489): the
    // 260 records stating Qrr = 0 are sicSchottky/schottky, where zero is the measurement
    // — no minority charge to recover — and nz() would report the one class of diode
    // whose recovery IS its selling point as having no recovery data at all.
    // A zener's Vz is matched, not floored: the ranker rejects one whose Vz lies outside
    // the original's breakdown band (Qarlos 2026-11-08), so the pool is cut at that edge
    // instead of at 0.9x Vrrm — otherwise 800 rows of wrong-voltage zeners fill the pool
    // and the genuine equivalents never reach the ranker.
    variantFor: (orig) => (orig?.technology === 'zener' ? ZENER_VARIANT : null),
    spec: (r) => ({ ...base(r), vrrm: nz(r.vrrm_rated), if_avg: nz(r.if_avg_rated), vf: nz(r.vf_typ),
      power_dissipation: nz(r.power_dissipation),
      qrr: r.qrr ?? null, trr: r.trr ?? null, technology: r.technology ?? '',
      // The stated die configuration fixes the basis of if_avg (a 'dual' package's
      // forwardCurrent is per leg): the ranker compares it only like with like and leaves
      // it UNVERIFIED when one side is multi-die and the other states no configuration.
      // null when unstated, never guessed from the MPN.
      die_configuration: r.die_configuration || null,
      internal_connection: r.internal_connection || null,
      vz_min_V: nz(r.vz_min), vz_max_V: nz(r.vz_max),
      vz_tolerance_pct: r.vz_tolerance != null ? r.vz_tolerance * 100 : null,
      // tvs / esd protection specs, each with the impulse it is stated at: the ranker
      // compares a surge rating only against one stated at the SAME waveform
      // (ABT #2036, #2040, #2041, #2048-#2051). Empty waveform = not stated.
      clamping_voltage: nz(r.clamping_voltage), clamping_current: nz(r.clamping_current),
      peak_pulse_power: nz(r.peak_pulse_power), peak_pulse_current: nz(r.peak_pulse_current),
      junction_capacitance: nz(r.junction_capacitance),
      clamping_waveform: r.clamping_waveform ?? '',
      peak_pulse_power_waveform: r.peak_pulse_power_waveform ?? '',
      peak_pulse_current_waveform: r.peak_pulse_current_waveform ?? '' }),
    params: [
      { key: 'vrrm', label: 'Vrrm', row: 'vrrm_rated', unit: 'V' },
      { key: 'if_avg', label: 'If avg', row: 'if_avg_rated', unit: 'A' },
      { key: 'vf', label: 'Vf', row: 'vf_typ', unit: 'V' },
      { key: 'qrr', label: 'Qrr', row: 'qrr', unit: 'C' },
      { key: 'vz_tolerance_pct', label: 'Vbr tol', row: 'vz_tolerance', unit: '%', scale: 100 },
      { key: 'power_dissipation', label: 'Pd', row: 'power_dissipation', unit: 'W' },
      { key: 'clamping_voltage', label: 'Vc', row: 'clamping_voltage', unit: 'V' },
      { key: 'peak_pulse_power', label: 'Ppk', row: 'peak_pulse_power', unit: 'W' },
      { key: 'peak_pulse_current', label: 'Ipp', row: 'peak_pulse_current', unit: 'A' },
      { key: 'junction_capacitance', label: 'Cj', row: 'junction_capacitance', unit: 'F' },
    ],
  },
]

// Zener view of the diode catalogue: same comparison model, but the pool is cut at the
// ranker's own zener-voltage reject edge (src/CrossRef.hpp, kZenerEdgeTol / kZenerPointTol):
// the substitute's Vz (vrrm_rated: its nominal, else its band midpoint) inside the
// original's stated band, or within 1 % of its Vz when it states no band. Nothing the
// ranker would keep is dropped.
const ZENER_EDGE_TOL = 1e-3
const ZENER_POINT_TOL = 0.01
const ZENER_VARIANT = {
  hardMin: [],
  rangeFilters: (orig) => {
    const lo = nz(orig.vz_min), hi = nz(orig.vz_max), v = nz(orig.vrrm_rated)
    if (lo != null && hi != null && hi >= lo) {
      return { vrrm_rated: { min: lo * (1 - ZENER_EDGE_TOL), max: hi * (1 + ZENER_EDGE_TOL) } }
    }
    if (v != null) return { vrrm_rated: { min: v * (1 - ZENER_POINT_TOL), max: v * (1 + ZENER_POINT_TOL) } }
    return {}
  },
}

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
    // A leaded body's lead pitch (m): its land pattern, which the body outline is not.
    // null when unstated — the footprint check then cannot pass a leaded part on its box.
    lead_pitch_m: nz(r.leadPitchM),
    is_production: r.is_production,
    // Every provenance entry of the record is retracted: the ranker treats its data as
    // unverified (ABT #2035).
    provenance_retracted: r.provenanceRetracted === true,
    // A leaded L x W x T body with no height whose record does not say which of L and W
    // stands up from the board: the row carries no box and the ranker reports the
    // footprint UNVERIFIED with a note (v20).
    box_axes_ambiguous: r.boxAxesAmbiguous === true,
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
//
// The rule is expressed ONCE, over spec keys, and projected to display labels for
// prose. A consumer that must decide whether to present an unverified cross-reference
// at all has to match on something stable: 'saturation_current' survives someone
// improving the table, 'Isat' does not.
export function originalMissingKeys(f, spec) {
  const miss = []
  if (f.primary && spec.value_si == null) miss.push('value_si')
  for (const k of f.hardKeys) {
    if (spec[k] == null) miss.push(k)
  }
  return miss
}

// The same absences, named the way the comparison table names them.
export function originalMissing(f, spec) {
  return originalMissingKeys(f, spec).map((k) =>
    (k === 'value_si'
      ? (f.primary?.label ?? k)
      : (f.params.find((p) => p.key === k)?.label ?? k)))
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
    if (h.onlyWithout && nz(original[h.onlyWithout]) != null) continue
    const o = nz(original[h.row])
    if (o != null) filters[h.row] = { min: o * h.factor }
  }
  Object.assign(filters, f.rangeFilters?.(original) ?? {})
  // IDENTITY parameters constrain the pool rather than just failing later: a
  // 25 MHz crystal is not a near-match for a 48 MHz one, so ranking it and
  // rejecting it fills the table with 25 rows of "no substitute" and buries
  // the parts that could actually work.
  for (const e of f.exactNum ?? []) {
    const v = nz(original[e.row])
    if (v != null) filters[e.row] = { min: v * (1 - e.tol), max: v * (1 + e.tol) }
  }
  for (const k of f.sameStr ?? []) {
    if (original[k]) filters[k] = [original[k]]
  }
  if (sameType && f.sameFacet && original[f.sameFacet.f]) {
    filters[f.sameFacet.f] = [original[f.sameFacet.f]]
  }

  const pool = await browse(f.key, { filters, sort: { field: 'lineno', dir: 'asc' }, limit: poolLimit })
  const rows = pool.rows.filter((r) => !(r.mpn === original.mpn && r.manufacturer === original.manufacturer))
  const rowByKey = new Map(rows.map((r) => [keyOf(r), r]))
  const missing = originalMissing(f, origSpec)
  // An original whose every source is retracted is not a verified spec either (ABT #2035).
  const origVerified = missing.length === 0 && original.provenanceRetracted !== true
  const ranked = rows.length
    ? (await crossReference(f.category, { ...origSpec, _key: keyOf(original) },
        rows.map((r) => ({ ...f.spec(r), _key: keyOf(r) })),
        { original_verified: origVerified, max_results: Number(maxResults) })).candidates
    : []
  return { fam: f, origSpec, ranked, rowByKey, poolTotal: pool.total,
           poolScored: rows.length, origVerified, missing,
           originalRetracted: original.provenanceRetracted === true }
}
