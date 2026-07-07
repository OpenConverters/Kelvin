// SI-engineering formatting + unit-aware input parsing ("4.7u", "4u7", "10 mΩ", "2k2").

const PREFIXES = [
  { exp: 12, sym: 'T' }, { exp: 9, sym: 'G' }, { exp: 6, sym: 'M' },
  { exp: 3, sym: 'k' }, { exp: 0, sym: '' }, { exp: -3, sym: 'm' },
  { exp: -6, sym: 'µ' }, { exp: -9, sym: 'n' }, { exp: -12, sym: 'p' },
  { exp: -15, sym: 'f' },
]

// 4.7e-6, 'H' -> "4.7 µH"; 0.052, 'Ω' -> "52 mΩ"; null -> "—"
export function si(value, unit = '', digits = 3) {
  if (value == null || Number.isNaN(value)) return '—'
  if (value === 0) return unit ? `0 ${unit}` : '0'
  const sign = value < 0 ? '-' : ''
  const abs = Math.abs(value)
  let p = PREFIXES[PREFIXES.length - 1]
  for (const cand of PREFIXES) {
    if (abs >= Math.pow(10, cand.exp)) { p = cand; break }
  }
  const scaled = abs / Math.pow(10, p.exp)
  // exponent fell outside the table (ultra small/large): plain exponential
  if (scaled >= 1000 || !Number.isFinite(scaled)) return `${sign}${abs.toExponential(2)} ${unit}`.trim()
  // Number() round-trip kills both trailing zeros and toPrecision's "1.0e+2" form
  const s = String(Number(scaled.toPrecision(digits)))
  return `${sign}${s} ${p.sym}${unit}`.trim()
}

const MULT = {
  T: 1e12, G: 1e9, M: 1e6, k: 1e3, K: 1e3,
  m: 1e-3, u: 1e-6, 'µ': 1e-6, 'μ': 1e-6, n: 1e-9, p: 1e-12, f: 1e-15,
}

// "4.7u" | "4u7" | "10 m" | "2k2" | "3.3" -> number (unit letters like H/F/A/V/Ω/Hz ignored).
// Returns null for empty, NaN for unparseable (the form shows the error, never guesses).
export function parseSI(text) {
  if (text == null) return null
  let s = String(text).trim()
  if (s === '') return null
  // strip unit words/symbols but keep prefix letters: remove trailing unit chars
  s = s.replace(/(ohm|ohms|hz|Ω|ω|[HFAVWs])+$/i, (m) => {
    // careful: a trailing 'm' is a PREFIX (milli) not a unit — the regex above never
    // eats 'm'/'u'/'k' etc. because they're not in the class; only whole-unit tokens.
    return ''
  }).trim()
  // forms: 12.3, 12.3k, 4u7, 2k2
  let m = s.match(/^([+-]?\d+(?:\.\d+)?)(?:\s*([TGMkKmuµμnpf]))?$/)
  if (m) {
    const mult = m[2] ? MULT[m[2]] : 1
    return parseFloat(m[1]) * mult
  }
  m = s.match(/^([+-]?\d+)([TGMkKmuµμnpf])(\d+)$/) // 4u7 / 2k2
  if (m) {
    const mult = MULT[m[2]]
    return parseFloat(`${m[1]}.${m[3]}`) * mult
  }
  m = s.match(/^([+-]?\d+(?:\.\d+)?[eE][+-]?\d+)$/)
  if (m) return parseFloat(m[1])
  return NaN
}

export function pct(x, digits = 0) {
  if (x == null || Number.isNaN(x)) return '—'
  return `${(x * 100).toFixed(digits)}%`
}
