<script setup>
// Compare: the pinned set becomes overlay charts (every curve type any pinned part
// carries) + a side-by-side spec table with differing rows highlighted. Each part
// keeps its pin color everywhere — table, legend, traces.
import { computed, ref, watchEffect } from 'vue'
import { store, record, clearPins } from '../store.js'
import { extractCurves, specRows } from '../curves.js'
import { familyByKey } from '../families.js'
import { si } from '../units.js'
import CurveChart from '../components/CurveChart.vue'

const loaded = ref([]) // [{pin, record, curves, specs}]
const errs = ref([])
const loading = ref(false)

watchEffect(async () => {
  const pins = [...store.pins]
  loaded.value = []
  errs.value = []
  if (!pins.length) return
  loading.value = true
  const out = await Promise.all(pins.map(async (pin) => {
    try {
      const rec = await record(pin.family, pin.srcOffset, pin.srcLength)
      return { pin, record: rec, curves: extractCurves(rec), specs: specRows(rec) }
    } catch (e) {
      errs.value.push(`${pin.mpn}: ${e.message}`)
      return null
    }
  }))
  loaded.value = out.filter(Boolean)
  loading.value = false
})

// merge: one overlay chart per curve (key,title) present on ≥1 part; a part
// contributes its first series of that curve (multi-temperature curves note it).
const overlays = computed(() => {
  const byKey = new Map()
  for (const item of loaded.value) {
    for (const c of item.curves) {
      const k = `${c.key}::${c.title}`
      if (!byKey.has(k)) byKey.set(k, { ...c, series: [] })
      const src = c.series[0]
      if (!src) continue
      const name = c.series.length > 1 && src.name
        ? `${item.pin.mpn} (${src.name})`
        : item.pin.mpn
      byKey.get(k).series.push({ name, points: src.points, color: item.pin.color })
    }
  }
  return [...byKey.values()]
})

// spec diff: union of (group,key) across parts; a row is "diff" when values differ
const specTable = computed(() => {
  const keys = new Map() // `${group}|${key}` -> {group,key}
  for (const item of loaded.value) {
    for (const row of item.specs) keys.set(`${row.group}|${row.key}`, { group: row.group, key: row.key })
  }
  const rows = []
  for (const { group, key } of keys.values()) {
    const values = loaded.value.map((item) => {
      const r = item.specs.find((s) => s.group === group && s.key === key)
      if (!r) return null
      return typeof r.value === 'number' ? si(r.value, '', 4) : String(r.value)
    })
    const present = values.filter((v) => v != null)
    rows.push({ group, key, values, diff: new Set(present).size > 1 })
  }
  return rows
})

const onlyDiff = ref(true)
const visibleSpecs = computed(() => (onlyDiff.value ? specTable.value.filter((r) => r.diff) : specTable.value))

const familyLabel = computed(() =>
  store.pins.length ? familyByKey(store.pins[0].family)?.label : '')
</script>

<template>
  <div class="compare">
    <div v-if="!store.pins.length" class="empty panel">
      <p class="section-label">Compare</p>
      <p>Nothing pinned yet. In the catalogue or the recommender, hit <span class="mono">◉</span> on a part —
        up to six parts overlay their datasheet curves here, each keeping its color.</p>
    </div>

    <template v-else>
      <header class="cmp-head">
        <p class="section-label">Compare — {{ familyLabel }} · {{ store.pins.length }} pinned</p>
        <button type="button" @click="clearPins">clear tray</button>
      </header>

      <p v-for="e in errs" :key="e" class="err mono">{{ e }}</p>
      <p v-if="loading" class="note">Reading records… (one Range slice each)</p>

      <section v-if="overlays.length" class="panel pane">
        <p class="section-label">Curve overlays</p>
        <div class="overlay-grid">
          <CurveChart v-for="c in overlays" :key="c.key + c.title" :curve="c" :height="280" />
        </div>
      </section>
      <p v-else-if="!loading && loaded.length" class="note">
        None of the pinned parts carries curve data — the spec table below is the comparison.
      </p>

      <section v-if="loaded.length" class="panel pane">
        <div class="spec-head">
          <p class="section-label">Spec sheet, side by side</p>
          <label class="check">
            <input v-model="onlyDiff" type="checkbox" />
            <span>differences only</span>
          </label>
        </div>
        <div class="spec-scroll">
          <table>
            <thead>
              <tr>
                <th>spec</th>
                <th v-for="item in loaded" :key="item.pin.mpn">
                  <i class="dot" :style="{ background: item.pin.color }" />
                  <span class="mono">{{ item.pin.mpn }}</span>
                  <span class="mfr">{{ item.pin.manufacturer }}</span>
                </th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="row in visibleSpecs" :key="row.group + row.key" :class="{ diff: row.diff }">
                <td class="spec-key">{{ row.key }} <span class="grp">{{ row.group }}</span></td>
                <td v-for="(v, i) in row.values" :key="i" class="mono val">{{ v ?? '—' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.compare { display: flex; flex-direction: column; gap: 14px; flex: 1; min-height: 0; overflow-y: auto; }
.empty { padding: 24px; color: var(--ink-dim); font-size: 13px; max-width: 560px; }
.cmp-head { display: flex; justify-content: space-between; align-items: center; }
.pane { padding: 16px; }
.overlay-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(420px, 780px)); justify-content: start; gap: 18px; }
.note { color: var(--ink-dim); font-size: 12px; }
.err { color: var(--fault); font-size: 12px; }
.spec-head { display: flex; justify-content: space-between; align-items: center; }
.check { display: flex; align-items: center; gap: 7px; font-size: 11px; color: var(--ink-dim); cursor: pointer; }
.check input { accent-color: var(--k); }
.spec-scroll { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-size: 12px; }
th {
  text-align: left;
  padding: 6px 10px;
  border-bottom: 1px solid var(--line);
  font-size: 11px;
  color: var(--ink);
  white-space: nowrap;
}
th .mfr { display: block; font-size: 9px; color: var(--ink-dim); font-family: var(--body); }
td { padding: 4px 10px; border-bottom: 1px solid var(--line-soft); white-space: nowrap; }
.spec-key { color: var(--ink-dim); }
.grp { font-size: 9px; color: #4a5b6e; margin-left: 6px; }
tr.diff .val { color: var(--k-hi); }
tr.diff .spec-key { color: var(--ink); }
.dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; margin-right: 6px; }
</style>
