<script setup>
// Recommend: designRequirements in → Kelvin's deterministic SelectionResult out.
// Every candidate is auditable: per-spec null-balance meters, evidence badges, and
// on zero candidates the rejection histogram (what the gate dropped, and why).
import { computed, reactive, ref, watch } from 'vue'
import { browse, select } from '../engine.js'
import { FAMILIES, familyByKey, MARGIN_LABELS } from '../families.js'
import { parseSI, si } from '../units.js'
import { store, togglePin, isPinned, pinColor } from '../store.js'
import { trackEvent } from '../telemetry.js'
import MarginMeter from '../components/MarginMeter.vue'

// browse-only families have no selector — the recommender only lists selectable ones
const SELECTABLE = FAMILIES.filter((f) => f.recommend)
const fam = computed(() => {
  const f = familyByKey(store.family)
  return f?.recommend ? f : SELECTABLE[0]
})

// form state survives family switches
const forms = reactive({})
function form() {
  if (!forms[fam.value.key]) {
    forms[fam.value.key] = {
      text: {}, // field key -> raw text
      role: false,
      technology: [],
      facet: {},
      tiebreaker: '',
      topology: '',
      inputVoltage: '',
      switchingFrequency: '',
      diversity: true,
      allowlist: [],
      maxCandidates: 25,
      loss: {}, // lossOperatingPoint field key -> raw text
      priceQuantity: '',
      lossPosition: '', // lossOperatingPoint.position when the family offers several
    }
  }
  return forms[fam.value.key]
}

const result = ref(null)
const errorMsg = ref('')
const busy = ref(false)
const ran = ref(false)

watch(() => fam.value.key, () => { result.value = null; errorMsg.value = ''; ran.value = false })

// select options come from the live shard facets, not hardcoded lists
const topologies = ref([])
const facetOptions = reactive({}) // facet field -> [values]
watch(() => fam.value.key, async (k) => {
  const wantsTopo = k === 'controller'
  const facetFields = fam.value.recommend.facetFields ?? []
  if (!wantsTopo && !facetFields.length) return
  try {
    const r = await browse(k, { withFacets: true, limit: 0 })
    if (wantsTopo) topologies.value = r.facets.topologies.values.map(([v]) => v)
    for (const ff of facetFields) {
      facetOptions[ff.facet] = r.facets[ff.facet].values.map(([v]) => v).filter(Boolean)
    }
  } catch { /* facet lists are a convenience; select still validates */ }
}, { immediate: true })

function parsedField(field) {
  const raw = form().text[field.key]
  if (raw == null || String(raw).trim() === '') return null
  const v = parseSI(raw)
  if (v == null || Number.isNaN(v)) return NaN
  return field.scale ? v * field.scale : v
}

// The operating-point loss inputs the user filled in (blank = not given, never defaulted).
function lossPointValues() {
  const out = {}
  for (const field of fam.value.recommend.lossPoint?.fields ?? []) {
    const raw = form().loss[field.key]
    if (raw == null || String(raw).trim() === '') continue
    const v = parseSI(raw)
    out[field.key] = v == null ? NaN : v
  }
  return out
}

const formErrors = computed(() => {
  const errs = []
  const f = fam.value
  for (const field of f.recommend.fields ?? []) {
    const v = parsedField(field)
    if (Number.isNaN(v)) errs.push(`${field.label}: can't parse "${form().text[field.key]}"`)
    else if (v == null && field.required) errs.push(`${field.label} is required`)
  }
  if (f.recommend.lossPoint) {
    const lp = lossPointValues()
    for (const [k, v] of Object.entries(lp)) {
      if (Number.isNaN(v)) errs.push(`${k}: can't parse "${form().loss[k]}"`)
    }
    const given = Object.keys(lp).length
    const core = ['inputVoltage', 'outputCurrent', 'switchingFrequency']
    if (given && core.some((k) => lp[k] == null)) {
      errs.push('Operating-point loss needs Vin, Iout and fs')
    }
    if (given && lp.outputVoltage == null && lp.dutyCycle == null) {
      errs.push('Operating-point loss needs Vout or a duty cycle')
    }
    const position = form().lossPosition || f.recommend.lossPoint.position
    for (const field of f.recommend.lossPoint.fields) {
      if (given && field.requiredFor === position && lp[field.key] == null) {
        errs.push(`Operating-point loss at ${position} needs ${field.label}`)
      }
      if (field.onlyFor && field.onlyFor !== position && lp[field.key] != null) {
        errs.push(`${field.label} applies to ${field.onlyFor} only`)
      }
    }
  }
  if (f.recommend.priceQuantity && form().priceQuantity !== '') {
    const q = parseSI(form().priceQuantity)
    if (q == null || Number.isNaN(q) || !(q > 0)) errs.push('Price quantity must be a positive number')
  }
  if (f.recommend.contextRequired) {
    if (!form().topology) errs.push('Topology is required')
    if (Number.isNaN(parseSI(form().inputVoltage)) || parseSI(form().inputVoltage) == null) errs.push('Input voltage is required')
    if (Number.isNaN(parseSI(form().switchingFrequency)) || parseSI(form().switchingFrequency) == null) errs.push('Switching frequency is required')
  }
  return errs
})

async function run() {
  const f = fam.value
  const st = form()
  const req = {}
  const options = { maxCandidates: Number(st.maxCandidates) || 25 }

  for (const field of f.recommend.fields ?? []) {
    const v = parsedField(field)
    if (v == null || Number.isNaN(v)) continue
    if (field.dimArray) req[field.dimArray] = [{ nominal: v }]
    else if (field.dim) req[field.key] = { nominal: v }
    else req[field.key] = v
  }
  for (const opt of f.recommend.options ?? []) {
    const raw = st.text[opt.key]
    const v = parseSI(raw)
    if (v != null && !Number.isNaN(v)) options[opt.key] = v
  }
  if (f.recommend.roleToggle && st.role) req[f.recommend.roleToggle.key] = f.recommend.roleToggle.value
  for (const ff of f.recommend.facetFields ?? []) {
    if (st.facet[ff.key]) req[ff.key] = st.facet[ff.key]
  }
  if (f.recommend.technologyOption && st.technology.length) options.technologyAllowed = st.technology
  if (st.tiebreaker) options.tiebreaker = st.tiebreaker
  if (f.recommend.contextRequired) {
    options.topology = st.topology
    options.inputVoltage = parseSI(st.inputVoltage)
    options.switchingFrequency = parseSI(st.switchingFrequency)
  }
  if (f.recommend.lossPoint) {
    const lp = lossPointValues()
    if (Object.keys(lp).length) options.lossOperatingPoint = { position: st.lossPosition || f.recommend.lossPoint.position, ...lp }
  }
  if (f.recommend.priceQuantity && st.priceQuantity !== '') options.priceQuantity = parseSI(st.priceQuantity)
  if (st.diversity) options.maxManufacturerFraction = 0.2
  if (st.allowlist.length) options.manufacturerAllowlist = st.allowlist

  busy.value = true
  errorMsg.value = ''
  ran.value = true
  trackEvent('recommend_run', {
    target: fam.value.key,
    topology: st.topology || undefined,
    tiebreaker: st.tiebreaker || undefined,
    diversity: !!st.diversity,
    allowlist: st.allowlist.length || undefined,
  })
  try {
    result.value = await select(fam.value.key, req, options)
    trackEvent('recommend_result', {
      target: fam.value.key,
      candidates: result.value?.candidates?.length ?? 0,
      error: result.value?.error || undefined,
    })
  } catch (e) {
    result.value = null
    errorMsg.value = e.message
    trackEvent('recommend_error', { target: fam.value.key, message: (e.message || '').slice(0, 120) })
  } finally {
    busy.value = false
  }
}

const candidates = computed(() => result.value?.candidates ?? [])
const rejections = computed(() => {
  const r = result.value?.rejections
  if (!r) return []
  return Object.entries(r).sort((a, b) => b[1] - a[1])
})
const maxRejection = computed(() => Math.max(1, ...rejections.value.map(([, n]) => n)))

// magnetic verdictByDimension -> margin-key verdict override
const DIM_FOR_MARGIN = {
  inductance_ratio: 'inductance',
  saturation_headroom: 'saturationCurrent',
  rated_headroom: 'ratedCurrent',
  turns_ratio_ratio: 'turnsRatio',
}
function meterRows(c) {
  const out = []
  for (const [key, value] of Object.entries(c.margins ?? {})) {
    if (key.endsWith('_log_distance')) continue // internal ranking detail, not a spec margin
    out.push({
      key,
      label: MARGIN_LABELS[key] ?? key,
      value: typeof value === 'number' ? value : null,
      kind: key.endsWith('_ratio') ? 'ratio' : 'headroom',
      verdict: c.verdictByDimension?.[DIM_FOR_MARGIN[key]] ?? null,
    })
  }
  return out
}

function evidenceBadges(c) {
  const e = c.evidence ?? {}
  const out = []
  if (e.thermalPresent === false) out.push({ t: 'θ?', title: 'no thermal data in the datasheet record' })
  if (e.datasheetUsable === false) out.push({ t: '⚠ sheet', title: 'datasheet link unusable' })
  if (e.qgPresent === false) out.push({ t: 'Qg?', title: 'gate charge not documented' })
  if (e.isProduction === false) out.push({ t: 'EOL?', title: 'not marked production status' })
  return out
}

function openPart(c) {
  if (typeof c.srcOffset !== 'number') return
  store.drawer = { family: fam.value.key, mpn: c.mpn, manufacturer: c.manufacturer, srcOffset: c.srcOffset, srcLength: c.srcLength }
}

const target = computed(() => result.value?.target ?? null)

// Side-by-side comparison (owner 2026-11-14): the operating-point loss terms, the over-rating
// notes, board area and price of the first candidates, one column each. Every cell carries
// its basis as a tooltip; an UNVERIFIED term says what is missing instead of a number.
const COMPARE_COLUMNS = 6
const compareCands = computed(() => candidates.value.slice(0, COMPARE_COLUMNS))
const showCompare = computed(() => compareCands.value.some((c) => c.loss || c.boardArea || c.price))
const LOSS_TERMS = [
  ['conduction', 'Conduction'],
  ['switching', 'Switching'],
  ['gateDrive', 'Gate drive'],
  ['coss', 'Coss'],
  ['esrLoss', 'ESR loss'],
]
// A family states its own loss terms and ratings row (families.js); the MOSFET ones otherwise.
const lossTerms = computed(() => fam.value.recommend?.lossTerms ?? LOSS_TERMS)
const ratingsRow = computed(() => fam.value.recommend?.ratingsRow ?? { label: 'Vds / Id', keys: [['vds', 'V'], ['id', 'A']] })
function termCell(c, key) {
  const t = c.loss?.terms?.[key]
  if (!t) return { text: '—', title: '' }
  if (t.watts == null) return { text: 'UNVERIFIED', title: (t.missing ?? []).join('; '), unverified: true }
  // ESR derived from tan(delta) (owner 2026-11-14): an upper bound, shown as one.
  return { text: (t.upperBound ? '≤ ' : '') + si(t.watts, 'W'), title: t.basis ?? '' }
}
function totalCell(c) {
  if (!c.loss) return { text: '—', title: '' }
  if (c.loss.total == null) {
    return {
      text: `≥ ${si(c.loss.computedSum, 'W')}`,
      title: `incomplete: ${(c.loss.unverified ?? []).join(', ')} not computed`,
      unverified: true,
    }
  }
  return { text: si(c.loss.total, 'W'), title: (c.loss.notes ?? []).join('; ') }
}
function areaCell(c) {
  const a = c.boardArea
  if (!a) return { text: '—', title: '' }
  if (a.mm2 == null) return { text: 'not stated', title: a.basis ?? '', unverified: true }
  return { text: `${a.mm2.toFixed(1)} mm²`, title: a.basis ?? '' }
}
// Capacitor checks (owner 2026-11-14): Ceff, ripple voltage, ripple-current rating at the
// operating point, and the volume (height matters for a capacitor).
function checkCell(c, key, unitKey, unit) {
  const t = c.loss?.checks?.[key]
  if (!t) return { text: '—', title: '' }
  if (t[unitKey] == null) return { text: 'UNVERIFIED', title: (t.missing ?? []).join('; '), unverified: true }
  let text = si(t[unitKey], unit)
  let bad = false
  if (key === 'rippleVoltage' && t.limit != null) { text += t.overLimit ? ' > limit' : ' ≤ limit'; bad = t.overLimit }
  if (key === 'rippleCurrentRating') { text += ` (need ${si(t.needed, 'A')})`; bad = t.meets === false }
  return { text, title: t.basis ?? '', unverified: bad }
}
function volumeCell(c) {
  const v = c.volume
  if (!v) return { text: '—', title: '' }
  if (v.mm3 == null) return { text: 'not stated', title: v.basis ?? '', unverified: true }
  return { text: `${v.mm3.toFixed(0)} mm³`, title: v.basis ?? '' }
}
const CAP_CHECKS = [
  ['effectiveCapacitance', 'farads', 'F', 'Ceff at Vdc'],
  ['rippleVoltage', 'volts', 'V', 'Ripple voltage'],
  ['rippleCurrentRating', 'amperes', 'A', 'Ripple rating @ op'],
]
function priceCell(c) {
  const p = c.price
  if (!p) return { text: '—', title: '' }
  const q = (p.quotes ?? [])[0]
  if (p.atQuantity != null) return { text: `${p.atQuantity.toFixed(2)} ${p.currency}`, title: p.basis ?? '' }
  if (q) {
    return {
      text: `${q.unitCost.toFixed(2)} ${q.currency} (unit)`,
      title: `${q.distributor}${q.moq != null ? `, MOQ ${q.moq}` : ''} — ${p.basis ?? ''}`,
      unverified: true,
    }
  }
  return { text: 'not stated', title: p.basis ?? '', unverified: true }
}
</script>

<template>
  <div class="recommend">
    <div class="family-strip">
      <button
        v-for="f in SELECTABLE" :key="f.key"
        class="fam-tab" :class="{ active: f.key === fam.key }"
        type="button"
        @click="store.family = f.key"
      >{{ f.label }}</button>
    </div>

    <div class="split">
      <!-- requirement panel -->
      <section class="req panel">
        <p class="section-label">Requirements — {{ fam.label }}</p>
        <p v-if="fam.recommend.intro" class="intro">{{ fam.recommend.intro }}</p>

        <template v-if="fam.recommend.contextRequired">
          <label class="field">
            <span>Topology <em>required</em></span>
            <select v-model="form().topology">
              <option value="" disabled>choose a topology</option>
              <option v-for="t in topologies" :key="t" :value="t">{{ t }}</option>
            </select>
          </label>
          <label class="field">
            <span>Input voltage (V) <em>required</em></span>
            <input v-model="form().inputVoltage" placeholder="48" />
          </label>
          <label class="field">
            <span>Switching frequency (Hz) <em>required</em></span>
            <input v-model="form().switchingFrequency" placeholder="100k" />
          </label>
        </template>

        <label v-for="field in fam.recommend.fields" :key="field.key" class="field">
          <span>{{ field.label }} <template v-if="field.unit">({{ field.unit }})</template> <em v-if="field.required">required</em></span>
          <input v-model="form().text[field.key]" :placeholder="field.placeholder" />
        </label>

        <label v-for="ff in fam.recommend.facetFields ?? []" :key="ff.key" class="field">
          <span>{{ ff.label }}</span>
          <select v-model="form().facet[ff.key]">
            <option value="">any</option>
            <option v-for="v in facetOptions[ff.facet] ?? []" :key="v" :value="v">{{ v }}</option>
          </select>
        </label>

        <label v-for="opt in fam.recommend.options ?? []" :key="opt.key" class="field">
          <span>{{ opt.label }} <template v-if="opt.unit">({{ opt.unit }})</template></span>
          <input v-model="form().text[opt.key]" :placeholder="opt.placeholder" />
        </label>

        <fieldset v-if="fam.recommend.lossPoint" class="loss-point">
          <legend>Operating-point loss <span class="dim">({{ form().lossPosition || fam.recommend.lossPoint.position }})</span></legend>
          <label v-if="fam.recommend.lossPoint.positions" class="field">
            <span>Position</span>
            <select v-model="form().lossPosition">
              <option value="">{{ fam.recommend.lossPoint.position }}</option>
              <option v-for="p in fam.recommend.lossPoint.positions.filter((x) => x !== fam.recommend.lossPoint.position)" :key="p" :value="p">{{ p }}</option>
            </select>
          </label>
          <label v-for="lf in fam.recommend.lossPoint.fields" :key="lf.key" class="field">
            <span>{{ lf.label }} <template v-if="lf.unit">({{ lf.unit }})</template></span>
            <input v-model="form().loss[lf.key]" :placeholder="lf.placeholder" />
          </label>
        </fieldset>

        <label v-if="fam.recommend.priceQuantity" class="field">
          <span>Price at quantity</span>
          <input v-model="form().priceQuantity" placeholder="1000" />
        </label>

        <label v-if="fam.recommend.roleToggle" class="check">
          <input v-model="form().role" type="checkbox" />
          <span>{{ fam.recommend.roleToggle.label }}</span>
        </label>

        <div v-if="fam.recommend.technologyOption" class="field">
          <span class="f-span">Technology</span>
          <div class="tech-row">
            <label v-for="t in fam.recommend.technologyOption" :key="t" class="check inline">
              <input v-model="form().technology" type="checkbox" :value="t" />
              <span>{{ t }}</span>
            </label>
          </div>
        </div>

        <label v-if="fam.recommend.tiebreakers.length" class="field">
          <span>Rank by</span>
          <select v-model="form().tiebreaker">
            <option value="">default</option>
            <option v-for="t in fam.recommend.tiebreakers" :key="t" :value="t">{{ t.replaceAll('_', ' ') }}</option>
          </select>
        </label>

        <label class="check">
          <input v-model="form().diversity" type="checkbox" />
          <span>Vendor diversity (cap any one manufacturer at 20%)</span>
        </label>

        <label class="field">
          <span>Candidates</span>
          <select v-model="form().maxCandidates">
            <option :value="10">10</option>
            <option :value="25">25</option>
            <option :value="50">50</option>
          </select>
        </label>

        <ul v-if="ran && formErrors.length" class="form-errs">
          <li v-for="e in formErrors" :key="e">{{ e }}</li>
        </ul>

        <button class="primary run" type="button" :disabled="busy" @click="formErrors.length ? null : run()">
          {{ busy ? 'balancing…' : 'Find parts' }}
        </button>

        <div v-if="result?.manufacturers?.length" class="field">
          <span class="f-span">Restrict manufacturers <span class="dim">(re-run applies)</span></span>
          <select v-model="form().allowlist" multiple size="5">
            <option v-for="m in result.manufacturers" :key="m" :value="m">{{ m }}</option>
          </select>
          <button v-if="form().allowlist.length" class="more" type="button" @click="form().allowlist = []">clear restriction</button>
        </div>
      </section>

      <!-- candidates -->
      <section class="cands panel">
        <p class="section-label">
          Candidates
          <template v-if="result && !result.error">
            — <span class="mono">{{ candidates.length }}</span> ranked
            · <span class="mono">{{ result.totalRowsConsidered?.toLocaleString() }}</span> considered
            <template v-if="result.tiebreaker"> · rank: <span class="mono">{{ result.tiebreaker }}</span></template>
          </template>
        </p>

        <p v-if="target" class="target mono">
          target:
          <template v-if="target.inductance"> L {{ si(target.inductance, 'H') }}</template>
          <template v-if="target.peakCurrent"> · I peak {{ si(target.peakCurrent, 'A') }}</template>
          <template v-if="target.rmsCurrent"> · I rms {{ si(target.rmsCurrent, 'A') }}</template>
          <template v-if="target.turnsRatio"> · n {{ target.turnsRatio }}</template>
          <template v-if="target.kind"> · {{ target.kind }}</template>
        </p>

        <p v-if="errorMsg" class="err mono">{{ errorMsg }}</p>

        <div v-if="!ran" class="idle">
          <p>Set the requirement and <b>Find parts</b>. Selection runs the same deterministic
          Kelvin engine Kirchhoff and Heaviside use — same question, same answer.</p>
        </div>

        <!-- NoCandidates: the rejection histogram is the answer, not a failure page -->
        <div v-else-if="result?.error === 'NoCandidates'" class="rejections">
          <p class="rej-title">No part passes every gate.
            <span class="mono">{{ result.totalRowsConsidered?.toLocaleString() }}</span> parts considered — what the gates dropped:</p>
          <div v-for="[key, n] in rejections" :key="key" class="rej-row">
            <span class="rej-key mono">{{ key }}</span>
            <span class="rej-bar"><i :style="{ width: `${(n / maxRejection) * 100}%` }" /></span>
            <span class="rej-n mono">{{ n.toLocaleString() }}</span>
          </div>
          <p class="hint">Loosen the dominant gate above and re-run.</p>
        </div>

        <template v-else-if="candidates.length">
        <div v-if="showCompare" class="compare-wrap">
          <table class="compare mono">
            <thead>
              <tr>
                <th />
                <th v-for="c in compareCands" :key="c.mpn">{{ c.mpn }}<br><span class="dim">{{ c.manufacturer }}</span></th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="fam.key !== 'capacitor'">
                <th>{{ ratingsRow.label }}</th>
                <td v-for="c in compareCands" :key="c.mpn">{{ ratingsRow.keys.map(([k, u]) => si(c.ratings?.[k], u)).join(' / ') }}</td>
              </tr>
              <tr v-if="fam.key === 'capacitor'">
                <th>C / V rated</th>
                <td v-for="c in compareCands" :key="c.mpn" :title="[c.ratings?.technology, c.ratings?.dielectric].filter(Boolean).join(' ')">{{ si(c.ratings?.capacitance, 'F') }} / {{ si(c.ratings?.voltage, 'V') }}</td>
              </tr>
              <tr>
                <th>Over-rated</th>
                <td v-for="c in compareCands" :key="c.mpn" :title="(c.overRating?.notes ?? []).join('; ')" :class="{ warnc: c.overRating?.axes?.length }">
                  {{ c.overRating?.axes?.length ? c.overRating.axes.join(', ') : 'no' }}
                </td>
              </tr>
              <template v-if="compareCands.some((c) => c.loss)">
                <tr v-for="[k, label] in lossTerms.filter(([t]) => compareCands.some((c) => c.loss?.terms?.[t]))" :key="k">
                  <th>{{ label }}</th>
                  <td v-for="c in compareCands" :key="c.mpn" :title="termCell(c, k).title" :class="{ warnc: termCell(c, k).unverified }">{{ termCell(c, k).text }}</td>
                </tr>
                <tr class="total">
                  <th>Total loss</th>
                  <td v-for="c in compareCands" :key="c.mpn" :title="totalCell(c).title" :class="{ warnc: totalCell(c).unverified }">{{ totalCell(c).text }}</td>
                </tr>
                <template v-if="compareCands.some((c) => c.loss?.checks)">
                  <tr v-for="[k, uk, unit, label] in CAP_CHECKS.filter(([t]) => compareCands.some((c) => c.loss?.checks?.[t]))" :key="k">
                    <th>{{ label }}</th>
                    <td v-for="c in compareCands" :key="c.mpn" :title="checkCell(c, k, uk, unit).title" :class="{ warnc: checkCell(c, k, uk, unit).unverified }">{{ checkCell(c, k, uk, unit).text }}</td>
                  </tr>
                </template>
              </template>
              <tr>
                <th>Board area</th>
                <td v-for="c in compareCands" :key="c.mpn" :title="areaCell(c).title" :class="{ warnc: areaCell(c).unverified }">{{ areaCell(c).text }}</td>
              </tr>
              <tr v-if="compareCands.some((c) => c.volume)">
                <th>Volume</th>
                <td v-for="c in compareCands" :key="c.mpn" :title="volumeCell(c).title" :class="{ warnc: volumeCell(c).unverified }">{{ volumeCell(c).text }}</td>
              </tr>
              <tr>
                <th>Price<template v-if="compareCands[0]?.price?.quantity"> @ {{ compareCands[0].price.quantity }}</template></th>
                <td v-for="c in compareCands" :key="c.mpn" :title="priceCell(c).title" :class="{ warnc: priceCell(c).unverified }">{{ priceCell(c).text }}</td>
              </tr>
            </tbody>
          </table>
          <p class="hint">Hover a cell for its basis. UNVERIFIED = the record (or the operating point) lacks what the term needs; nothing is estimated.</p>
        </div>

        <ol class="cand-list">
          <li v-for="(c, i) in candidates" :key="c.mpn" class="cand">
            <header class="cand-head">
              <span class="rank mono">{{ String(i + 1).padStart(2, '0') }}</span>
              <button class="mpn-link mono" type="button" @click="openPart(c)">{{ c.mpn }}</button>
              <span class="mfr">{{ c.manufacturer }}</span>
              <span v-if="c.verdict" class="chip" :class="`verdict-${c.verdict}`">{{ c.verdict }}</span>
              <span v-if="c.evidence?.family" class="chip">{{ c.evidence.family }}</span>
              <span v-if="c.evidence?.positions != null" class="chip">{{ c.evidence.positions }} pos</span>
              <span v-if="c.evidence?.dieConfiguration" class="chip" title="multi-die package: ratings are per die">{{ c.evidence.dieConfiguration }}{{ c.evidence.internalConnection ? ` · ${c.evidence.internalConnection}` : '' }}</span>
              <span v-for="b in evidenceBadges(c)" :key="b.t" class="chip warn" :title="b.title">{{ b.t }}</span>
              <button
                class="pin"
                type="button"
                :style="isPinned(c.mpn) ? { color: pinColor(c.mpn), borderColor: pinColor(c.mpn) } : {}"
                @click="togglePin(fam.key, c)"
              >◉</button>
            </header>
            <div v-if="meterRows(c).length" class="meters">
              <MarginMeter
                v-for="m in meterRows(c)" :key="m.key"
                :label="m.label" :value="m.value" :kind="m.kind" :verdict="m.verdict"
              />
            </div>
          </li>
        </ol>
        </template>

        <p v-else-if="ran && result && !busy" class="idle">Selection returned no candidate list.</p>
      </section>
    </div>
  </div>
</template>

<style scoped>
.recommend { display: flex; flex-direction: column; gap: 14px; flex: 1; min-height: 0; }
.family-strip { display: flex; flex-wrap: wrap; gap: 6px; }
.fam-tab {
  font-family: var(--disp);
  font-size: 10px;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  padding: 7px 12px;
}
.fam-tab.active { border-color: var(--k-deep); color: var(--k-hi); box-shadow: 0 0 10px rgba(127, 201, 255, 0.1) inset; }

.split { display: grid; grid-template-columns: 330px 1fr; gap: 14px; flex: 1; min-height: 0; }
.req { padding: 16px; display: flex; flex-direction: column; gap: 12px; overflow-y: auto; max-height: calc(100vh - 210px); }
.intro { font-size: 11px; color: var(--ink-dim); margin: 0; }
.field { display: flex; flex-direction: column; gap: 4px; font-size: 11px; }
.field span, .f-span { color: var(--ink-dim); }
.field em { color: var(--warm); font-style: normal; font-size: 10px; }
.field .dim { color: #4a5b6e; }
.check { display: flex; align-items: center; gap: 8px; font-size: 11px; color: var(--ink-dim); cursor: pointer; }
.check input { accent-color: var(--k); }
.check.inline { display: inline-flex; }
.tech-row { display: flex; gap: 14px; margin-top: 4px; }
.run { margin-top: 4px; }
.form-errs { margin: 0; padding-left: 18px; color: var(--fault); font-size: 11px; }
.more { font-size: 10px; padding: 2px 8px; align-self: flex-start; margin-top: 4px; }

.cands { padding: 16px; overflow-y: auto; max-height: calc(100vh - 210px); }
.target { font-size: 11px; color: var(--ink-dim); margin: 0 0 10px; }
.idle { color: var(--ink-dim); font-size: 12px; max-width: 460px; }
.err { color: var(--fault); font-size: 12px; }

.cand-list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 10px; }
.cand { border: 1px solid var(--line-soft); border-radius: 6px; padding: 10px 12px; background: var(--bg-deep); }
.cand-head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.rank { color: var(--k-deep); font-size: 11px; }
.mpn-link { background: none; border: none; padding: 0; color: var(--k-hi); font-size: 13px; cursor: pointer; }
.mpn-link:hover { text-decoration: underline; color: var(--k-hi); }
.mfr { color: var(--ink-dim); font-size: 11px; flex: 1; }
.chip.warn { color: var(--warm); border-color: rgba(255, 179, 71, 0.4); }
.pin { padding: 1px 7px; border-radius: 999px; font-size: 12px; }
.meters { margin-top: 8px; display: flex; flex-direction: column; }

.rejections { max-width: 560px; }
.rej-title { font-size: 12px; color: var(--ink); }
.rej-row { display: grid; grid-template-columns: 180px 1fr 70px; gap: 10px; align-items: center; padding: 2px 0; }
.rej-key { font-size: 10px; color: var(--ink-dim); overflow: hidden; text-overflow: ellipsis; }
.rej-bar { height: 10px; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 3px; overflow: hidden; }
.rej-bar i { display: block; height: 100%; background: var(--warm); opacity: 0.75; }
.rej-n { font-size: 10px; text-align: right; color: var(--ink); }
.hint { font-size: 11px; color: var(--ink-dim); }
.loss-point { border: 1px solid var(--line-soft); border-radius: 6px; padding: 8px 10px; display: flex; flex-direction: column; gap: 8px; }
.loss-point legend { font-size: 11px; color: var(--ink-dim); padding: 0 4px; }
.compare-wrap { overflow-x: auto; margin-bottom: 12px; }
.compare { border-collapse: collapse; font-size: 11px; min-width: 100%; }
.compare th, .compare td { border: 1px solid var(--line-soft); padding: 4px 8px; text-align: right; white-space: nowrap; }
.compare thead th { text-align: center; color: var(--k-hi); }
.compare tbody th { text-align: left; color: var(--ink-dim); font-weight: normal; }
.compare tr.total td { color: var(--k-hi); }
.compare .warnc { color: var(--warm); }
.compare .dim { color: var(--ink-dim); }

@media (max-width: 1100px) {
  .split { grid-template-columns: 1fr; }
  .req, .cands { max-height: none; }
}
</style>
