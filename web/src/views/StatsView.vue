<script setup>
// Stats — the observatory. Everything here is computed live in the WASM engine over
// the loaded shard (facets, ranges, log-histograms), so the charts always describe
// exactly the data the catalogue serves: distribution of every spec, vendor share,
// technology mix, production status, and the librarian's own data-coverage numbers.
import { computed, ref, watch } from 'vue'
import { browse } from '../engine.js'
import { FAMILIES, familyByKey } from '../families.js'
import { si, pct } from '../units.js'
import { store } from '../store.js'

const props = defineProps({
  counts: { type: Object, default: () => ({}) }, // family -> row count (manifest)
})

const fam = computed(() => familyByKey(store.family))
const base = ref(null)      // facets + ranges + total
const prodTotal = ref(null) // production-only total (null = family has no status flag)
const hists = ref([])       // [{field, label, unit, log, edges, counts, present, absent}]
const busy = ref(false)
const error = ref('')

const numericCols = computed(() => fam.value.columns.filter((c) => !c.str && !c.bool))

let token = 0
watch(() => store.family, load, { immediate: true })

async function load() {
  const t = ++token
  busy.value = true
  error.value = ''
  base.value = null
  prodTotal.value = null
  hists.value = []
  try {
    const b = await browse(store.family, { withFacets: true, facetTop: 1000, limit: 0 })
    if (t !== token) return
    base.value = b

    try {
      const p = await browse(store.family, { filters: { is_production: true }, limit: 0 })
      if (t !== token) return
      prodTotal.value = p.total
    } catch { prodTotal.value = null } // family carries no status flag (e.g. controllers)

    const out = []
    for (const col of numericCols.value) {
      const r = b.ranges?.[col.f]
      if (!r || r.present === 0) continue
      // decide the axis from the data: >2 decades → log buckets
      const log = r.min > 0 && r.max / r.min >= 100
      const h = await browse(store.family, {
        histogram: { field: col.f, buckets: 28, log },
        limit: 0,
      })
      if (t !== token) return
      if (h.histogram?.counts) {
        out.push({ ...h.histogram, label: col.label, unit: col.unit, scale: col.scale ?? 1, plain: col.plain })
      }
    }
    hists.value = out
  } catch (e) {
    if (t === token) error.value = e.message
  } finally {
    if (t === token) busy.value = false
  }
}

// ── tiles ─────────────────────────────────────────────────────────────────────
const mfrCount = computed(() => {
  const f = base.value?.facets?.manufacturer
  return f ? f.values.length + f.omitted : null
})
const prodShare = computed(() => {
  if (prodTotal.value == null || !base.value?.total) return null
  return prodTotal.value / base.value.total
})
const coverage = computed(() => {
  // spec coverage: how much of the catalogue actually documents each numeric field
  if (!base.value?.ranges) return []
  return numericCols.value
    .map((c) => ({ label: c.label, frac: (base.value.ranges[c.f]?.present ?? 0) / base.value.total }))
    .filter((x) => Number.isFinite(x.frac))
})

// ── bar lists (vendor share, facet mixes) ─────────────────────────────────────
const TOP = 12
function barList(field) {
  const f = base.value?.facets?.[field]
  if (!f || !f.values.length) return null
  const top = f.values.slice(0, TOP)
  const shown = top.reduce((a, [, n]) => a + n, 0)
  const rest = (base.value.total - shown)
  const max = Math.max(...top.map(([, n]) => n), 1)
  return {
    rows: top.map(([v, n]) => ({ label: v || '—', n, w: n / max })),
    rest: rest > 0 ? rest : 0,
    restDistinct: f.values.length - top.length + f.omitted,
  }
}
const vendorBars = computed(() => barList('manufacturer'))
const facetBars = computed(() =>
  fam.value.facets
    .filter((fc) => fc.f !== 'series') // thousands of series values — the facet rail covers it
    .map((fc) => ({ ...fc, bars: barList(fc.f) }))
    .filter((x) => x.bars))

// ── histogram svg helpers ─────────────────────────────────────────────────────
const HW = 760
const HH = 150
const HPAD = { l: 10, r: 10, t: 8, b: 20 }
function bars(h) {
  const n = h.counts.length
  const max = Math.max(...h.counts, 1)
  const w = (HW - HPAD.l - HPAD.r) / n
  return h.counts.map((c, i) => ({
    x: HPAD.l + i * w + 1,
    w: Math.max(1, w - 2),
    y: HH - HPAD.b - (c / max) * (HH - HPAD.t - HPAD.b),
    h: (c / max) * (HH - HPAD.t - HPAD.b),
    c,
    lo: h.edges[i],
    hi: h.edges[i + 1],
  }))
}
function edgeTicks(h) {
  const idx = [0, Math.round(h.counts.length / 2), h.counts.length]
  return idx.map((i) => ({
    x: HPAD.l + (i / h.counts.length) * (HW - HPAD.l - HPAD.r),
    v: h.edges[i],
    anchor: i === 0 ? 'start' : i === h.counts.length ? 'end' : 'middle',
  }))
}
function fmtEdge(h, v) {
  const s = v * h.scale
  return h.plain ? `${Number(s.toPrecision(2))}${h.unit ? ` ${h.unit}` : ''}` : si(s, h.unit, 2)
}
const hoverBucket = ref(null) // {histField, i, ...bar}
</script>

<template>
  <div class="stats">
    <div class="family-strip">
      <button
        v-for="f in FAMILIES" :key="f.key"
        class="fam-tab" :class="{ active: f.key === store.family }"
        type="button"
        @click="store.family = f.key"
      >{{ f.label }}</button>
    </div>

    <p v-if="error" class="err mono">{{ error }}</p>

    <template v-if="base">
      <!-- tiles -->
      <div class="tiles">
        <div class="tile panel">
          <span class="tile-label">catalogued parts</span>
          <span class="tile-value mono">{{ base.total.toLocaleString() }}</span>
        </div>
        <div class="tile panel">
          <span class="tile-label">manufacturers</span>
          <span class="tile-value mono">{{ mfrCount ?? '—' }}</span>
        </div>
        <div class="tile panel" v-if="prodShare != null">
          <span class="tile-label">production status</span>
          <span class="tile-value mono">{{ pct(prodShare, 1) }}</span>
        </div>
        <div class="tile panel wide" v-if="coverage.length">
          <span class="tile-label">spec coverage — share of parts documenting each value</span>
          <div class="cov-row">
            <span v-for="c in coverage" :key="c.label" class="cov">
              <span class="cov-label">{{ c.label }}</span>
              <span class="cov-track"><i :style="{ width: pct(c.frac) }" /></span>
              <span class="cov-pct mono">{{ pct(c.frac) }}</span>
            </span>
          </div>
        </div>
      </div>

      <div class="grid">
        <!-- vendor share -->
        <section v-if="vendorBars" class="panel pane">
          <p class="section-label">Manufacturer share</p>
          <div v-for="row in vendorBars.rows" :key="row.label" class="bar-row">
            <span class="bar-label" :title="row.label">{{ row.label }}</span>
            <span class="bar-track"><i :style="{ width: pct(row.w) }" /></span>
            <span class="bar-n mono">{{ row.n.toLocaleString() }}</span>
          </div>
          <p v-if="vendorBars.rest" class="bar-rest">
            + {{ vendorBars.rest.toLocaleString() }} parts across {{ vendorBars.restDistinct }} more manufacturers
          </p>
        </section>

        <!-- facet mixes -->
        <section v-for="fb in facetBars" :key="fb.f" class="panel pane">
          <p class="section-label">{{ fb.label }} mix</p>
          <div v-for="row in fb.bars.rows" :key="row.label" class="bar-row">
            <span class="bar-label" :title="row.label">{{ row.label }}</span>
            <span class="bar-track"><i :style="{ width: pct(row.w) }" /></span>
            <span class="bar-n mono">{{ row.n.toLocaleString() }}</span>
          </div>
          <p v-if="fb.bars.rest" class="bar-rest">
            + {{ fb.bars.rest.toLocaleString() }} parts in {{ fb.bars.restDistinct }} more values
          </p>
        </section>
      </div>

      <!-- spec distributions -->
      <section v-if="hists.length" class="panel pane">
        <p class="section-label">Spec distributions <span class="dim">— computed live over the loaded shard; undocumented parts counted as absent</span></p>
        <div class="hist-grid">
          <figure v-for="h in hists" :key="h.field" class="hist">
            <figcaption>
              <span>{{ h.label }} <template v-if="h.unit">({{ h.unit }})</template></span>
              <span class="hist-note mono">{{ h.log ? 'log' : 'lin' }} · {{ h.present.toLocaleString() }} documented<template v-if="h.absent"> · {{ h.absent.toLocaleString() }} absent</template></span>
            </figcaption>
            <svg :viewBox="`0 0 ${HW} ${HH}`" preserveAspectRatio="xMidYMid meet" @mouseleave="hoverBucket = null">
              <rect
                v-for="(b, i) in bars(h)" :key="i"
                :x="b.x" :y="b.y" :width="b.w" :height="Math.max(b.h, b.c ? 1 : 0)"
                class="hbar" :class="{ hot: hoverBucket?.histField === h.field && hoverBucket?.i === i }"
                @mouseenter="hoverBucket = { histField: h.field, i, ...b }"
              />
              <g class="ticks">
                <text v-for="t in edgeTicks(h)" :key="t.x" :x="t.x" :y="HH - 5" :text-anchor="t.anchor">
                  {{ fmtEdge(h, t.v) }}
                </text>
              </g>
            </svg>
            <p class="hist-readout mono">
              <template v-if="hoverBucket?.histField === h.field">
                {{ fmtEdge(h, hoverBucket.lo) }} – {{ fmtEdge(h, hoverBucket.hi) }} : {{ hoverBucket.c.toLocaleString() }} parts
              </template>
              <template v-else>&nbsp;</template>
            </p>
          </figure>
        </div>
      </section>
    </template>
    <p v-else-if="busy" class="note">Surveying the {{ fam.label.toLowerCase() }} shard…</p>
  </div>
</template>

<style scoped>
.stats { display: flex; flex-direction: column; gap: 14px; flex: 1; min-height: 0; overflow-y: auto; }
.family-strip { display: flex; flex-wrap: wrap; gap: 6px; }
.fam-tab {
  font-family: var(--disp);
  font-size: 10px;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  padding: 7px 12px;
}
.fam-tab.active { border-color: var(--k-deep); color: var(--k-hi); box-shadow: 0 0 10px rgba(127, 201, 255, 0.1) inset; }
.err { color: var(--fault); font-size: 12px; }
.note { color: var(--ink-dim); font-size: 12px; }

.tiles { display: flex; flex-wrap: wrap; gap: 10px; }
.tile { padding: 12px 18px; display: flex; flex-direction: column; gap: 4px; }
.tile.wide { flex: 1; min-width: 340px; }
.tile-label { font-family: var(--disp); font-size: 9px; letter-spacing: 0.18em; text-transform: uppercase; color: var(--ink-dim); }
.tile-value { font-size: 22px; color: var(--k-hi); }
.cov-row { display: flex; flex-wrap: wrap; gap: 6px 18px; }
.cov { display: inline-flex; align-items: center; gap: 7px; font-size: 11px; }
.cov-label { color: var(--ink-dim); min-width: 52px; }
.cov-track { width: 90px; height: 8px; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 2px; overflow: hidden; }
.cov-track i { display: block; height: 100%; background: var(--k-deep); }
.cov-pct { font-size: 10px; color: var(--ink); }

.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 14px; }
.pane { padding: 16px; }
.dim { text-transform: none; letter-spacing: 0; color: #4a5b6e; }

.bar-row { display: grid; grid-template-columns: 150px 1fr 66px; gap: 10px; align-items: center; padding: 2px 0; font-size: 11px; }
.bar-label { color: var(--ink); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bar-track { height: 12px; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 3px; overflow: hidden; }
.bar-track i { display: block; height: 100%; background: var(--s1); border-radius: 2px 0 0 2px; }
.bar-n { text-align: right; color: var(--ink-dim); font-size: 10px; }
.bar-rest { font-size: 10px; color: var(--ink-dim); margin: 6px 0 0; }

.hist-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 16px; }
.hist { margin: 0; }
.hist figcaption { display: flex; justify-content: space-between; gap: 10px; font-size: 12px; color: var(--ink); margin-bottom: 4px; }
.hist-note { font-size: 10px; color: var(--ink-dim); }
.hist svg { width: 100%; display: block; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 4px; }
.hbar { fill: var(--s1); opacity: 0.85; }
.hbar.hot { fill: var(--k); opacity: 1; }
.ticks text { fill: var(--ink-dim); font-family: var(--mono); font-size: 10px; }
.hist-readout { font-size: 10px; color: var(--k-hi); min-height: 14px; margin: 3px 0 0; }
</style>
