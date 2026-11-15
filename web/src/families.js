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
      intro: 'Every part is ranked toward your targets. Nothing is dropped silently.',
      fields: [
        { key: 'inductance', label: 'Inductance', unit: 'H', dim: true, placeholder: '10u' },
        { key: 'peakCurrent', label: 'Peak current', unit: 'A', placeholder: '3' },
        { key: 'rmsCurrent', label: 'RMS current', unit: 'A', placeholder: '2' },
        { key: 'turnsRatio', label: 'Turns ratio (pri:sec)', unit: '', dimArray: 'turnsRatios', placeholder: '0.5 — transformers only' },
      ],
      required: [],
      tiebreakers: ['best_fit', 'lowest_operating_point_loss'],
      // Operating-point ranking of power inductors (owner 2026-11-14): options.lossOperatingPoint
      // with its conduction mode (required, never defaulted). CCM: the Inductance above is the
      // required minimum; DCM: the part's guaranteed maximum must stay below 0.9x the CCM/DCM
      // boundary at the minimum Vin (a 10 % margin); CrM: its tolerance band must lie inside the
      // window.
      // Saturation (20 % drop) and thermal (40 K rise) are hard gates; the DCR loss orders; core
      // loss is UNVERIFIED (no catalogue model). Nothing here is defaulted.
      lossPoint: {
        position: 'buckInductor',
        modeField: {
          key: 'conductionMode',
          label: 'Conduction mode',
          choices: [['ccm', 'CCM'], ['dcm', 'DCM'], ['crm', 'CrM / QR']],
        },
        requiredByMode: {
          ccm: ['inputVoltage', 'outputVoltage', 'outputCurrent', 'switchingFrequency'],
          dcm: ['inputVoltage', 'minimumInputVoltage', 'outputVoltage', 'outputCurrent', 'switchingFrequency'],
          crm: ['inputVoltage', 'outputVoltage', 'outputCurrent', 'minimumInductance', 'maximumInductance'],
        },
        fields: [
          { key: 'inputVoltage', label: 'Vin', unit: 'V', placeholder: '36' },
          { key: 'minimumInputVoltage', label: 'Vin min', unit: 'V', placeholder: '24 (DCM boundary)', modes: ['dcm'] },
          { key: 'outputVoltage', label: 'Vout', unit: 'V', placeholder: '12' },
          { key: 'outputCurrent', label: 'Iout', unit: 'A', placeholder: '2' },
          { key: 'switchingFrequency', label: 'fs', unit: 'Hz', placeholder: '80k', modes: ['ccm', 'dcm'] },
          { key: 'minimumInductance', label: 'L window min', unit: 'H', placeholder: '100u', modes: ['crm'] },
          { key: 'maximumInductance', label: 'L window max', unit: 'H', placeholder: '150u', modes: ['crm'] },
          { key: 'ambientTemperature', label: 'Ambient', unit: '°C', placeholder: 'optional (stated, not applied)' },
        ],
        terms: [['dcr', 'DCR loss'], ['coreLoss', 'Core / AC loss']],
        ratings: [['inductance', 'L', 'H'], ['saturationCurrent', 'Isat', 'A'], ['ratedCurrent', 'I rated', 'A'], ['dcr', 'DCR', 'Ω']],
        derived: [['inductanceAtPeak', 'L @ Ipk', 'H'], ['peakCurrent', 'Ipk', 'A'], ['rmsCurrent', 'Irms', 'A'], ['switchingFrequency', 'fs (CrM, from L)', 'Hz']],
        checks: [['inductance', 'Inductance (mode)'], ['saturation', 'Saturation (20 %)'], ['thermal', 'Thermal (40 K)']],
        volume: true,
      },
      priceQuantity: true,
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
      { f: 'v_rated_ac', label: 'V AC', unit: 'V' },
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
      tiebreakers: ['lowest_esr', 'highest_ripple_headroom', 'highest_voltage_margin', 'highest_capacitance', 'lowest_operating_point_loss'],
      // Operating-point ranking (owner 2026-11-14): options.lossOperatingPoint at the buck output
      // or input. Nothing is defaulted: a record lacking a value leaves its term UNVERIFIED.
      lossPoint: {
        position: 'buckOutput',
        positions: ['buckOutput', 'buckInput'],
        fields: [
          { key: 'inputVoltage', label: 'Vin', unit: 'V', placeholder: '36' },
          { key: 'outputVoltage', label: 'Vout (D = Vout/Vin)', unit: 'V', placeholder: '12' },
          { key: 'outputCurrent', label: 'Iout', unit: 'A', placeholder: '2' },
          { key: 'switchingFrequency', label: 'fs', unit: 'Hz', placeholder: '80k' },
          { key: 'inductorRippleCurrent', label: 'Inductor ripple (p-p)', unit: 'A', placeholder: 'required at the output', requiredFor: 'buckOutput' },
          { key: 'maxRippleVoltage', label: 'Max ripple voltage', unit: 'V', placeholder: 'optional' },
          { key: 'ambientTemperature', label: 'Ambient', unit: '°C', placeholder: 'moves the ripple rating' },
        ],
      },
      priceQuantity: true,
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
    facets: [{ f: 'technology', label: 'Technology' }, { f: 'channel', label: 'Channel' }, { f: 'die_configuration', label: 'Dies' }],
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
      tiebreakers: ['lowest_rds_on', 'lowest_qg', 'highest_vds_margin', 'highest_id_margin', 'lowest_total_loss', 'lowest_operating_point_loss'],
      // Operating-point loss (owner 2026-11-14): options.lossOperatingPoint. Input voltage,
      // output current and switching frequency enable it; nothing here is defaulted — a gate
      // drive left blank makes the switching / gate terms UNVERIFIED in the result.
      lossPoint: {
        position: 'buckHighSide',
        fields: [
          { key: 'inputVoltage', label: 'Vin', unit: 'V', placeholder: '36' },
          { key: 'outputVoltage', label: 'Vout (duty = Vout/Vin)', unit: 'V', placeholder: '12' },
          { key: 'dutyCycle', label: 'or duty cycle', unit: '', placeholder: '0.33' },
          { key: 'outputCurrent', label: 'Iout', unit: 'A', placeholder: '2' },
          { key: 'switchingFrequency', label: 'fs', unit: 'Hz', placeholder: '80k' },
          { key: 'inductorRippleCurrent', label: 'Inductor ripple (p-p)', unit: 'A', placeholder: 'optional' },
          { key: 'gateDriveVoltage', label: 'Gate drive voltage', unit: 'V', placeholder: 'needed for the gate term' },
          { key: 'gateDriveCurrent', label: 'Gate drive current', unit: 'A', placeholder: 'needed for the switching term' },
          { key: 'junctionTemperature', label: 'Tj', unit: '°C', placeholder: 'reads Rds(on)(Tj) when on record' },
        ],
      },
      priceQuantity: true,
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
      tiebreakers: ['lowest_vf', 'lowest_qrr', 'highest_vrrm_margin', 'highest_if_margin', 'lowest_operating_point_loss'],
      // Operating-point loss of the buck freewheel diode (owner 2026-11-14):
      // options.lossOperatingPoint. Nothing is defaulted — a value the record lacks makes its
      // term UNVERIFIED in the result.
      lossPoint: {
        position: 'buckFreewheel',
        fields: [
          { key: 'inputVoltage', label: 'Vin', unit: 'V', placeholder: '36' },
          { key: 'outputVoltage', label: 'Vout (duty = Vout/Vin)', unit: 'V', placeholder: '12' },
          { key: 'dutyCycle', label: 'or switch duty cycle', unit: '', placeholder: '0.33' },
          { key: 'outputCurrent', label: 'Iout', unit: 'A', placeholder: '2' },
          { key: 'switchingFrequency', label: 'fs', unit: 'Hz', placeholder: '80k' },
          { key: 'inductorRippleCurrent', label: 'Inductor ripple (p-p)', unit: 'A', placeholder: 'optional: follows the Vf(If) curve' },
          { key: 'junctionTemperature', label: 'Tj', unit: '°C', placeholder: 'optional: compared with the curves' },
        ],
      },
      // The side-by-side table's rows for this family.
      lossTerms: [['conduction', 'Conduction'], ['switching', 'Switching'], ['leakage', 'Leakage']],
      ratingsRow: { label: 'Vrrm / If(avg)', keys: [['vrrm', 'V'], ['ifAvg', 'A']] },
      priceQuantity: true,
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
    facets: [{ f: 'technology', label: 'Type' }, { f: 'polarity', label: 'Polarity' }],
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
  {
    key: 'connector',
    label: 'Connectors',
    tagline: 'board-to-board · headers · terminal blocks',
    glyph: 'J',
    columns: [
      { f: 'positions', label: 'pos', unit: '', plain: true },
      { f: 'pitch', label: 'pitch', unit: 'm' },
      { f: 'rated_current', label: 'I/contact', unit: 'A' },
      { f: 'rated_voltage', label: 'V rated', unit: 'V' },
      { f: 'family', label: 'Family', str: true },
      { f: 'polarity', label: 'Polarity', str: true },
    ],
    facets: [
      { f: 'family', label: 'Family' },
      { f: 'polarity', label: 'Polarity' },
      { f: 'interface_standard', label: 'Interface' },
      { f: 'contact_plating', label: 'Plating' },
      { f: 'series', label: 'Series' },
    ],
    recommend: {
      intro: 'Positions, family and polarity must match; ratings are minimums. Set at least one.',
      fields: [
        { key: 'positions', label: 'Positions (exact)', unit: '', placeholder: '26' },
        { key: 'minimumCurrentPerContact', label: 'Current per contact >=', unit: 'A', placeholder: '2' },
        { key: 'minimumRatedVoltage', label: 'Rated voltage >=', unit: 'V', placeholder: '250' },
      ],
      facetFields: [
        { key: 'family', label: 'Family', facet: 'family' },
        { key: 'matingPolarity', label: 'Mating polarity', facet: 'polarity' },
      ],
      tiebreakers: ['highest_current_margin', 'highest_voltage_margin'],
    },
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
  current_margin: 'I/contact margin',
  voltage_margin: 'V rated margin',
}
