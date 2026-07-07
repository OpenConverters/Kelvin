// Extract chartable curves + spec-sheet rows from a full TAS NDJSON record.
//
// Known curve carriers (from the TAS catalogs):
//   magnetic  electrical[i].impedancePoints    [{frequency, impedance:{magnitude}}...]
//             electrical[i].inductancePoints   [{current, inductance, temperature}...]
//             electrical[i].saturationCurrents [{percentInductanceDrop, current}...]
//   capacitor electrical.rippleCurrentFrequencyPoints   {xData:[Hz], yData:[factor]}
//             electrical.rippleCurrentTemperaturePoints {xData:[°C], yData:[factor]}
// plus a generic sweep: ANY {xData[], yData[]} pair or array-of-points value found in
// the datasheet's electrical/thermal blocks becomes a chart, so new curve keys in the
// TAS show up without a frontend release.

// ── record walking ───────────────────────────────────────────────────────────
// The family node is the object carrying manufacturerInfo (e.g. record.semiconductor.mosfet,
// record.magnetic, record.capacitor).
export function familyNode(record) {
  const seen = new Set()
  const walk = (node, depth) => {
    if (!node || typeof node !== 'object' || depth > 3 || seen.has(node)) return null
    seen.add(node)
    if (node.manufacturerInfo) return node
    for (const v of Object.values(node)) {
      const hit = walk(v, depth + 1)
      if (hit) return hit
    }
    return null
  }
  return walk(record, 0)
}

export function datasheetInfo(record) {
  return familyNode(record)?.manufacturerInfo?.datasheetInfo ?? null
}

const num = (x) => (typeof x === 'number' && Number.isFinite(x) ? x : null)

// dimensionWithTolerance-or-scalar -> display scalar (nominal → mid → max → min)
export function dimScalar(v) {
  if (typeof v === 'number') return v
  if (v && typeof v === 'object') {
    if (num(v.nominal) != null) return v.nominal
    if (num(v.minimum) != null && num(v.maximum) != null) return (v.minimum + v.maximum) / 2
    if (num(v.maximum) != null) return v.maximum
    if (num(v.minimum) != null) return v.minimum
  }
  return null
}

// ── curve defs ────────────────────────────────────────────────────────────────
// A curve: { key, title, xLabel, xUnit, yLabel, yUnit, xLog, yLog, series: [{name, points:[[x,y]...]}] }

function sorted(points) {
  return points.filter((p) => p[0] != null && p[1] != null).sort((a, b) => a[0] - b[0])
}

function fromXY(key, obj, meta) {
  const xs = obj?.xData, ys = obj?.yData
  if (!Array.isArray(xs) || !Array.isArray(ys) || xs.length < 2 || xs.length !== ys.length) return null
  const points = sorted(xs.map((x, i) => [num(x), num(ys[i])]))
  if (points.length < 2) return null
  return { key, ...meta, series: [{ name: null, points }] }
}

function spansDecades(points, idx, decades = 2) {
  const vals = points.map((p) => p[idx]).filter((v) => v != null && v > 0)
  if (vals.length < 2) return false
  return Math.log10(Math.max(...vals) / Math.min(...vals)) >= decades
}

function impedanceCurve(arr) {
  const points = sorted(arr.map((p) => [num(p?.frequency), num(p?.impedance?.magnitude ?? p?.impedance)]))
  if (points.length < 2) return null
  return {
    key: 'impedancePoints', title: 'Impedance vs frequency',
    xLabel: 'f', xUnit: 'Hz', yLabel: '|Z|', yUnit: 'Ω', xLog: true, yLog: true,
    series: [{ name: null, points }],
  }
}

function inductanceCurve(arr) {
  // group by temperature (one series per measured temperature, if present)
  const groups = new Map()
  for (const p of arr) {
    const x = num(p?.current), y = num(p?.inductance)
    if (x == null || y == null) continue
    const t = num(p?.temperature)
    const k = t == null ? '' : `${t} °C`
    if (!groups.has(k)) groups.set(k, [])
    groups.get(k).push([x, y])
  }
  const series = [...groups.entries()].map(([name, pts]) => ({ name: name || null, points: sorted(pts) }))
    .filter((s) => s.points.length >= 2)
  if (!series.length) return null
  return {
    key: 'inductancePoints', title: 'Inductance vs DC bias',
    xLabel: 'I', xUnit: 'A', yLabel: 'L', yUnit: 'H', xLog: false, yLog: false, series,
  }
}

function saturationCurve(arr) {
  const points = sorted(arr.map((p) => [num(p?.current), num(p?.percentInductanceDrop)]))
  if (!points.length) return null
  return {
    key: 'saturationCurrents', title: 'Inductance drop vs current',
    xLabel: 'I', xUnit: 'A', yLabel: 'ΔL', yUnit: '%', xLog: false, yLog: false,
    marks: true, // sparse datasheet table — draw the points, not just the line
    series: [{ name: null, points }],
  }
}

const XY_DEFS = {
  rippleCurrentFrequencyPoints: {
    title: 'Ripple-current multiplier vs frequency',
    xLabel: 'f', xUnit: 'Hz', yLabel: '×', yUnit: '', xLog: true, yLog: false,
  },
  rippleCurrentTemperaturePoints: {
    title: 'Ripple-current multiplier vs temperature',
    xLabel: 'T', xUnit: '°C', yLabel: '×', yUnit: '', xLog: false, yLog: false,
  },
}

const KNOWN_POINT_ARRAYS = {
  impedancePoints: impedanceCurve,
  inductancePoints: inductanceCurve,
  saturationCurrents: saturationCurve,
}

// generic array-of-points: every element an object sharing numeric keys; x is the
// first of (frequency,current,voltage,temperature) present, y the first other numeric.
function genericPointArray(key, arr) {
  if (arr.length < 2 || typeof arr[0] !== 'object' || arr[0] == null) return null
  const numericKeys = Object.keys(arr[0]).filter((k) => arr.every((p) => num(p?.[k]) != null))
  if (numericKeys.length < 2) return null
  const X_PREF = ['frequency', 'current', 'voltage', 'temperature', 'time']
  const xKey = X_PREF.find((k) => numericKeys.includes(k)) ?? numericKeys[0]
  const yKey = numericKeys.find((k) => k !== xKey)
  if (!yKey) return null
  const points = sorted(arr.map((p) => [num(p[xKey]), num(p[yKey])]))
  if (points.length < 2) return null
  return {
    key, title: prettify(key),
    xLabel: prettify(xKey), xUnit: unitFor(xKey), yLabel: prettify(yKey), yUnit: unitFor(yKey),
    xLog: spansDecades(points, 0), yLog: spansDecades(points, 1),
    series: [{ name: null, points }],
  }
}

function prettify(key) {
  return key.replace(/([a-z])([A-Z])/g, '$1 $2').replace(/^./, (c) => c.toUpperCase())
}
export function unitFor(key) {
  const k = key.toLowerCase()
  if (k.includes('frequency')) return 'Hz'
  if (k.includes('current')) return 'A'
  if (k.includes('voltage')) return 'V'
  if (k.includes('temperature')) return '°C'
  if (k.includes('inductance')) return 'H'
  if (k.includes('capacitance')) return 'F'
  if (k.includes('resistance') || k.includes('impedance') || k === 'esr') return 'Ω'
  if (k.includes('time')) return 's'
  if (k.includes('power') || k.includes('loss') || k.includes('dissipation')) return 'W'
  if (k.includes('charge')) return 'C'
  if (['length', 'width', 'height', 'diameter', 'pitch', 'thickness'].some((d) => k.includes(d))) return 'm'
  if (k.includes('weight') || k.includes('mass')) return 'g'
  return ''
}

// ── the extraction sweep ──────────────────────────────────────────────────────
function curvesFromBlock(block, suffix = '') {
  const out = []
  if (!block || typeof block !== 'object') return out
  for (const [key, val] of Object.entries(block)) {
    let curve = null
    if (val && typeof val === 'object' && !Array.isArray(val) && Array.isArray(val.xData)) {
      curve = fromXY(key, val, XY_DEFS[key] ?? {
        title: prettify(key),
        xLabel: 'x', xUnit: '', yLabel: prettify(key), yUnit: '',
        xLog: false, yLog: false,
      })
      if (curve && !XY_DEFS[key]) {
        curve.xLog = spansDecades(curve.series[0].points, 0)
        curve.yLog = spansDecades(curve.series[0].points, 1)
      }
    } else if (Array.isArray(val) && val.length >= 2) {
      curve = KNOWN_POINT_ARRAYS[key] ? KNOWN_POINT_ARRAYS[key](val) : genericPointArray(key, val)
    }
    if (curve) {
      if (suffix) curve.title += ` — ${suffix}`
      out.push(curve)
    }
  }
  return out
}

// All charts a record's datasheet supports. Magnetics: electrical is an ARRAY
// (per winding); other families: an object.
export function extractCurves(record) {
  const ds = datasheetInfo(record)
  if (!ds) return []
  const out = []
  const elec = ds.electrical
  if (Array.isArray(elec)) {
    elec.forEach((e, i) => out.push(...curvesFromBlock(e, elec.length > 1 ? `winding ${i + 1}` : '')))
  } else {
    out.push(...curvesFromBlock(elec))
  }
  out.push(...curvesFromBlock(ds.thermal))
  return out
}

// ── spec sheet rows: flatten scalar / dimensionWithTolerance leaves ───────────
export function specRows(record) {
  const ds = datasheetInfo(record)
  if (!ds) return []
  const rows = []
  const push = (group, key, v) => {
    if (typeof v === 'number' && Number.isFinite(v)) rows.push({ group, key, value: v })
    else if (typeof v === 'string' && v && v.length < 120) rows.push({ group, key, value: v })
    else if (typeof v === 'boolean') rows.push({ group, key, value: v ? 'yes' : 'no' })
    else if (v && typeof v === 'object' && !Array.isArray(v)) {
      const s = dimScalar(v)
      if (s != null) rows.push({ group, key, value: s, dim: v })
    }
  }
  const sweep = (group, block) => {
    if (!block || typeof block !== 'object') return
    for (const [k, v] of Object.entries(block)) push(group, k, v)
  }
  sweep('part', ds.part)
  const elec = ds.electrical
  if (Array.isArray(elec)) elec.forEach((e, i) => sweep(elec.length > 1 ? `electrical · winding ${i + 1}` : 'electrical', e))
  else sweep('electrical', elec)
  sweep('thermal', ds.thermal)
  sweep('mechanical', ds.mechanical)
  return rows
}
