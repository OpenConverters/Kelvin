// Per-family configuration: catalogue columns/filters (over the Browse.hpp shard
// fields) and the recommender form (the exact designRequirements keys + options
// Kelvin's Requirements.cpp reads — the UI never invents selection semantics).

export const FAMILIES = [
  {
    key: 'magnetic',
    label: 'Magnetics',
    tagline: 'inductors · transformers · chokes · beads',
    glyph: 'M',
    columns: [
      { f: 'inductance', label: 'L', unit: 'H' },
      { f: 'saturation_current', label: 'Isat', unit: 'A' },
      { f: 'rated_current', label: 'I rated', unit: 'A' },
      { f: 'dcr', label: 'DCR', unit: 'Ω' },
      { f: 'srf', label: 'SRF', unit: 'Hz' },
      { f: 'turns_ratio', label: 'n', unit: '' },
    ],
    facets: [
      { f: 'device_type', label: 'Type' },
      { f: 'family', label: 'Series' },
    ],
    recommend: {
      intro: 'Rank-not-gate: every part is ranked toward your targets; nothing is silently dropped.',
      fields: [
        { key: 'inductance', label: 'Inductance', unit: 'H', dim: true, placeholder: '10u' },
        { key: 'peakCurrent', label: 'Peak current', unit: 'A', placeholder: '3' },
        { key: 'rmsCurrent', label: 'RMS current', unit: 'A', placeholder: '2' },
        { key: 'turnsRatio', label: 'Turns ratio (pri:sec)', unit: '', dimArray: 'turnsRatios', placeholder: '0.5 — transformers only' },
      ],
      required: [],
      tiebreakers: [],
    },
  },
  {
    key: 'capacitor',
    label: 'Capacitors',
    tagline: 'MLCC · electrolytic · film',
    glyph: 'C',
    columns: [
      { f: 'capacitance', label: 'C', unit: 'F' },
      { f: 'v_rated', label: 'V rated', unit: 'V' },
      { f: 'ripple_current_rms', label: 'I ripple', unit: 'A' },
      { f: 'esr', label: 'ESR', unit: 'Ω' },
    ],
    facets: [{ f: 'technology', label: 'Technology' }],
    recommend: {
      fields: [
        { key: 'capacitance', label: 'Capacitance', unit: 'F', dim: true, required: true, placeholder: '4u7' },
        { key: 'ratedVoltage', label: 'Rated voltage ≥', unit: 'V', required: true, placeholder: '50' },
        { key: 'minimumRippleCurrent', label: 'Ripple current ≥', unit: 'A', placeholder: 'optional' },
      ],
      roleToggle: { key: 'role', value: 'resonant', label: 'Resonant duty (tight ±tolerance window instead of min…oversize)' },
      tiebreakers: ['lowest_esr', 'highest_ripple_headroom', 'highest_voltage_margin', 'highest_capacitance'],
    },
  },
  {
    key: 'mosfet',
    label: 'MOSFETs',
    tagline: 'Si · SiC · GaN switches',
    glyph: 'Q',
    columns: [
      { f: 'vds_rated', label: 'Vds', unit: 'V' },
      { f: 'id_continuous', label: 'Id', unit: 'A' },
      { f: 'rds_on', label: 'Rds(on)', unit: 'Ω' },
      { f: 'qg_total', label: 'Qg', unit: 'C' },
      { f: 'coss', label: 'Coss', unit: 'F' },
      { f: 'tj_max', label: 'Tj max', unit: '°C', plain: true },
    ],
    facets: [{ f: 'technology', label: 'Technology' }],
    recommend: {
      fields: [
        { key: 'ratedDrainSourceVoltage', label: 'Vds >=', unit: 'V', required: true, placeholder: '60' },
        { key: 'ratedContinuousDrainCurrent', label: 'Id >=', unit: 'A', required: true, placeholder: '5' },
        { key: 'maximumOnResistance', label: 'Rds(on) <=', unit: 'Ω', required: true, placeholder: '100m' },
      ],
      options: [
        { key: 'switchingFrequency', label: 'Switching frequency', unit: 'Hz', placeholder: 'enables total-loss ranking' },
      ],
      technologyOption: ['Si', 'SiC', 'GaN'],
      tiebreakers: ['lowest_rds_on', 'lowest_qg', 'highest_vds_margin', 'highest_id_margin', 'lowest_total_loss'],
    },
  },
  {
    key: 'diode',
    label: 'Diodes',
    tagline: 'schottky · fast recovery · rectifiers',
    glyph: 'D',
    columns: [
      { f: 'vrrm_rated', label: 'Vrrm', unit: 'V' },
      { f: 'if_avg_rated', label: 'If avg', unit: 'A' },
      { f: 'vf_typ', label: 'Vf', unit: 'V' },
      { f: 'qrr', label: 'Qrr', unit: 'C' },
      { f: 'trr', label: 'trr', unit: 's' },
    ],
    facets: [{ f: 'technology', label: 'Type' }],
    recommend: {
      fields: [
        { key: 'ratedReverseVoltage', label: 'Vrrm >=', unit: 'V', required: true, placeholder: '100' },
        { key: 'ratedForwardCurrent', label: 'If >=', unit: 'A', required: true, placeholder: '3' },
      ],
      options: [{ key: 'qrrMax', label: 'Qrr <=', unit: 'C', placeholder: 'optional', asOption: true }],
      tiebreakers: ['lowest_vf', 'lowest_qrr', 'highest_vrrm_margin', 'highest_if_margin'],
    },
  },
  {
    key: 'resistor',
    label: 'Resistors',
    tagline: 'precision · power · shunts',
    glyph: 'R',
    columns: [
      { f: 'resistance', label: 'R', unit: 'Ω' },
      { f: 'tolerance', label: 'tol', unit: '%', scale: 100, plain: true },
      { f: 'power_rating', label: 'P', unit: 'W' },
    ],
    facets: [],
    recommend: {
      fields: [
        { key: 'resistance', label: 'Resistance', unit: 'Ω', dim: true, required: true, placeholder: '2k2' },
        { key: 'tolerance', label: 'Tolerance ≤', unit: '%', scale: 0.01, placeholder: '5' },
      ],
      tiebreakers: [],
    },
  },
  {
    key: 'controller',
    label: 'Controllers',
    tagline: 'PWM · PFC · resonant control ICs',
    glyph: 'U',
    columns: [
      { f: 'vref', label: 'V ref', unit: 'V' },
      { f: 'category', label: 'Category', str: true },
      { f: 'integrated_fet', label: 'FET', bool: true },
      { f: 'integrated_driver', label: 'Driver', bool: true },
    ],
    facets: [
      { f: 'category', label: 'Category' },
      { f: 'topologies', label: 'Topology' },
    ],
    recommend: {
      intro: 'Controller matching runs on the converter context, not part limits.',
      contextRequired: true, // topology + inputVoltage + switchingFrequency go in options
      fields: [],
      tiebreakers: [],
    },
  },
  {
    key: 'igbt',
    label: 'IGBTs',
    tagline: 'high-voltage switches',
    glyph: 'G',
    columns: [
      { f: 'vces_rated', label: 'Vces', unit: 'V' },
      { f: 'ic_continuous', label: 'Ic', unit: 'A' },
      { f: 'vce_sat', label: 'Vce(sat)', unit: 'V' },
      { f: 'tj_max', label: 'Tj max', unit: '°C', plain: true },
    ],
    facets: [{ f: 'technology', label: 'Technology' }],
    recommend: {
      fields: [
        { key: 'ratedCollectorEmitterVoltage', label: 'Vces >=', unit: 'V', required: true, placeholder: '650' },
        { key: 'ratedCollectorCurrent', label: 'Ic >=', unit: 'A', required: true, placeholder: '20' },
        { key: 'maximumSaturationVoltage', label: 'Vce(sat) <=', unit: 'V', placeholder: 'optional' },
      ],
      tiebreakers: ['lowest_vce_sat', 'highest_vces_margin', 'highest_ic_margin'],
    },
  },
  {
    key: 'bjt',
    label: 'BJTs',
    tagline: 'bipolar transistors',
    glyph: 'T',
    columns: [
      { f: 'vceo_rated', label: 'Vceo', unit: 'V' },
      { f: 'ic_continuous', label: 'Ic', unit: 'A' },
      { f: 'hfe_min', label: 'hFE min', unit: '', plain: true },
      { f: 'power_dissipation', label: 'P', unit: 'W' },
    ],
    facets: [{ f: 'technology', label: 'Type' }],
    recommend: {
      fields: [
        { key: 'ratedCollectorEmitterVoltage', label: 'Vceo >=', unit: 'V', required: true, placeholder: '40' },
        { key: 'ratedCollectorCurrent', label: 'Ic >=', unit: 'A', required: true, placeholder: '0.5' },
        { key: 'minimumDcCurrentGain', label: 'hFE >=', unit: '', placeholder: 'optional' },
      ],
      tiebreakers: ['highest_hfe', 'highest_vceo_margin', 'highest_ic_margin'],
    },
  },
  {
    key: 'varistor',
    label: 'Varistors',
    tagline: 'MOV surge protection',
    glyph: 'V',
    columns: [
      { f: 'varistor_voltage', label: 'V varistor', unit: 'V' },
      { f: 'clamping_voltage', label: 'V clamp', unit: 'V' },
      { f: 'peak_surge_current', label: 'I surge', unit: 'A' },
      { f: 'capacitance', label: 'C', unit: 'F' },
    ],
    facets: [{ f: 'technology', label: 'Type' }],
    recommend: {
      fields: [
        { key: 'ratedContinuousVoltage', label: 'Continuous voltage ≥', unit: 'V', required: true, placeholder: '275' },
        { key: 'maximumClampingVoltage', label: 'Clamping ≤', unit: 'V', placeholder: 'optional' },
        { key: 'minimumPeakSurgeCurrent', label: 'Surge ≥', unit: 'A', placeholder: 'optional' },
        { key: 'maximumCapacitance', label: 'Capacitance ≤', unit: 'F', placeholder: 'optional' },
      ],
      tiebreakers: ['lowest_clamping_voltage', 'highest_surge', 'lowest_capacitance'],
    },
  },
]

// ---- browse-only families (no Kelvin selector yet — catalogue + graphs only) --
FAMILIES.push(
  {
    key: 'analog',
    label: 'Analog ICs',
    tagline: 'op-amps · comparators · ADC · DAC · switches',
    glyph: 'A',
    browseOnly: true,
    columns: [
      { f: 'channels', label: 'ch', unit: '', plain: true },
      { f: 'gain_bandwidth', label: 'GBW', unit: 'Hz' },
      { f: 'input_offset_voltage', label: 'Vos', unit: 'V' },
      { f: 'slew_rate', label: 'SR', unit: 'V/s' },
      { f: 'vsupply_max', label: 'Vsup max', unit: 'V' },
      { f: 'resolution', label: 'bits', unit: '', plain: true },
      { f: 'sample_rate', label: 'fs', unit: 'S/s' },
    ],
    facets: [
      { f: 'device_type', label: 'Type' },
      { f: 'architecture', label: 'Architecture' },
      { f: 'input_stage', label: 'Input stage' },
    ],
    recommend: null,
  },
  {
    key: 'timing',
    label: 'Timing',
    tagline: 'crystals · MEMS · XO · TCXO',
    glyph: 'X',
    browseOnly: true,
    columns: [
      { f: 'frequency', label: 'f', unit: 'Hz' },
      { f: 'frequency_tolerance', label: 'tol', unit: 'ppm', scale: 1e6, plain: true },
      { f: 'frequency_stability', label: 'stab', unit: 'ppm', scale: 1e6, plain: true },
      { f: 'load_capacitance', label: 'CL', unit: 'F' },
      { f: 'esr', label: 'ESR', unit: 'Ω' },
    ],
    facets: [
      { f: 'technology', label: 'Technology' },
      { f: 'output_type', label: 'Output' },
      { f: 'mode', label: 'Mode' },
    ],
    recommend: null,
  },
)

export function familyByKey(key) {
  return FAMILIES.find((f) => f.key === key) ?? null
}

// margins from SelectionResult candidates -> display metadata (label + polarity).
// Values are ratios/headroom fractions straight from Select.cpp; null = not computed.
export const MARGIN_LABELS = {
  vds_margin: 'Vds margin',
  id_margin: 'Id margin',
  rds_on_headroom: 'Rds(on) headroom',
  qg_headroom: 'Qg headroom',
  vrrm_margin: 'Vrrm margin',
  if_avg_margin: 'If margin',
  qrr_headroom: 'Qrr headroom',
  v_margin: 'Voltage margin',
  ripple_headroom: 'Ripple headroom',
  vces_margin: 'Vces margin',
  ic_margin: 'Ic margin',
  vce_sat_headroom: 'Vce(sat) headroom',
  vceo_margin: 'Vceo margin',
  vc_margin: 'V clamp margin',
  clamping_headroom: 'Clamping headroom',
  surge_headroom: 'Surge headroom',
  inductance_ratio: 'L fit (part/target)',
  saturation_headroom: 'Isat headroom',
  rated_headroom: 'I rated headroom',
  turns_ratio_ratio: 'n fit (part/target)',
}
