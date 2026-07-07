<script setup>
// Catalogue: family strip → filter rail + results table, all queries served by the
// WASM browse engine over the loaded shard (facet counts + numeric range hints
// included). Applied filters render as removable chips above the table.
import { computed, reactive, ref, watch } from 'vue'
import { browse } from '../engine.js'
import { FAMILIES, familyByKey } from '../families.js'
import { si } from '../units.js'
import { store, togglePin } from '../store.js'
import FilterRail from '../components/FilterRail.vue'
import PartsTable from '../components/PartsTable.vue'

const props = defineProps({
  counts: { type: Object, default: () => ({}) }, // family -> row count (manifest)
})

const family = computed(() => familyByKey(store.family))

const filters = ref({ mpn: '', num: {}, sel: {} })
const sort = ref({ field: 'lineno', dir: 'asc' })
const page = ref(0)
const PAGE = 50

const result = ref(null)
const busy = ref(true) // true until the first query lands — never flash "no parts match"
const error = ref('')

watch(() => store.family, () => {
  filters.value = { mpn: '', num: {}, sel: {} }
  sort.value = { field: 'lineno', dir: 'asc' }
  page.value = 0
})

function buildQuery() {
  const f = {}
  if (filters.value.mpn.trim()) f.mpn = filters.value.mpn.trim()
  for (const [field, range] of Object.entries(filters.value.num)) f[field] = range
  for (const [field, values] of Object.entries(filters.value.sel)) f[field] = values
  return {
    filters: f,
    sort: sort.value,
    offset: page.value * PAGE,
    limit: PAGE,
    withFacets: true,
    facetTop: 500,
  }
}

let queryToken = 0
async function run() {
  const token = ++queryToken
  busy.value = true
  error.value = ''
  try {
    const r = await browse(store.family, buildQuery())
    if (token !== queryToken) return
    result.value = r
  } catch (e) {
    if (token !== queryToken) return
    error.value = e.message
    result.value = null
  } finally {
    if (token === queryToken) busy.value = false
  }
}

let debounce = null
watch([filters, sort, page, () => store.family], () => {
  clearTimeout(debounce)
  debounce = setTimeout(run, 150)
}, { deep: true, immediate: true })

watch([filters, sort], () => { page.value = 0 }, { deep: true })

// applied-filter chips
const chips = computed(() => {
  const out = []
  const fam = family.value
  if (filters.value.mpn.trim()) out.push({ kind: 'mpn', label: `MPN ~ "${filters.value.mpn.trim()}"` })
  for (const [f, r] of Object.entries(filters.value.num)) {
    const col = fam.columns.find((c) => c.f === f)
    const parts = []
    if (r.min != null) parts.push(`≥ ${si(r.min * (col?.scale ?? 1), col?.unit ?? '')}`)
    if (r.max != null) parts.push(`≤ ${si(r.max * (col?.scale ?? 1), col?.unit ?? '')}`)
    out.push({ kind: 'num', field: f, label: `${col?.label ?? f} ${parts.join(' , ')}` })
  }
  for (const [f, vals] of Object.entries(filters.value.sel)) {
    for (const v of vals) out.push({ kind: 'sel', field: f, value: v, label: `${f === 'manufacturer' ? '' : f + ': '}${v || '—'}` })
  }
  return out
})

function removeChip(chip) {
  const next = JSON.parse(JSON.stringify(filters.value))
  if (chip.kind === 'mpn') next.mpn = ''
  else if (chip.kind === 'num') delete next.num[chip.field]
  else {
    next.sel[chip.field] = (next.sel[chip.field] ?? []).filter((v) => v !== chip.value)
    if (!next.sel[chip.field].length) delete next.sel[chip.field]
  }
  filters.value = next
}

function clearAll() {
  filters.value = { mpn: '', num: {}, sel: {} }
}

const totalPages = computed(() => (result.value ? Math.max(1, Math.ceil(result.value.total / PAGE)) : 1))

function openPart(row) {
  // note: ...row LAST would let MagneticRow's own `family` field (the series
  // string, e.g. "WE-CBF") clobber the category — keep the category authoritative
  store.drawer = { ...row, family: store.family }
}
</script>

<template>
  <div class="catalog">
    <!-- family strip: the card-catalogue drawers -->
    <div class="family-strip">
      <button
        v-for="f in FAMILIES"
        :key="f.key"
        class="fam-card"
        :class="{ active: f.key === store.family }"
        type="button"
        @click="store.family = f.key"
      >
        <span class="fam-glyph mono">{{ f.glyph }}</span>
        <span class="fam-name">{{ f.label }}</span>
        <span class="fam-count mono">{{ (counts[f.key] ?? 0).toLocaleString() }}</span>
        <span class="fam-tag">{{ f.tagline }}</span>
      </button>
    </div>

    <div class="workbench">
      <FilterRail
        v-model="filters"
        :family="family"
        :facets="result?.facets ?? null"
        :ranges="result?.ranges ?? null"
      />

      <section class="results panel">
        <div class="results-head">
          <p class="section-label">
            {{ family.label }} —
            <span class="mono">{{ result ? result.total.toLocaleString() : '…' }}</span> of
            <span class="mono">{{ (counts[store.family] ?? 0).toLocaleString() }}</span> parts
          </p>
          <div class="chips" v-if="chips.length">
            <button v-for="(chip, i) in chips" :key="i" class="chip on" type="button" @click="removeChip(chip)">
              {{ chip.label }} ✕
            </button>
            <button class="chip" type="button" @click="clearAll">clear all</button>
          </div>
        </div>

        <p v-if="error" class="err mono">{{ error }}</p>

        <PartsTable
          :family="family"
          :rows="result?.rows ?? []"
          :sort="sort"
          :busy="busy"
          @sort="sort = $event"
          @open="openPart"
          @pin="togglePin(store.family, $event)"
        />

        <footer class="pager">
          <button type="button" :disabled="page === 0" @click="page--">‹ prev</button>
          <span class="mono">page {{ page + 1 }} / {{ totalPages }}</span>
          <button type="button" :disabled="page + 1 >= totalPages" @click="page++">next ›</button>
        </footer>
      </section>
    </div>
  </div>
</template>

<style scoped>
.catalog { display: flex; flex-direction: column; gap: 14px; flex: 1; min-height: 0; }

.family-strip {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
  gap: 8px;
}
.fam-card {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 2px;
  padding: 8px 10px;
  text-align: left;
  background: linear-gradient(180deg, var(--panel-hi), var(--panel));
  border: 1px solid var(--line);
  border-radius: 6px;
}
.fam-card.active { border-color: var(--k-deep); box-shadow: 0 0 12px rgba(127, 201, 255, 0.12) inset; }
.fam-card.active .fam-name { color: var(--k-hi); }
.fam-glyph {
  font-size: 10px;
  color: var(--k);
  border: 1px solid var(--k-deep);
  border-radius: 3px;
  padding: 0 4px;
}
.fam-name { font-family: var(--disp); font-size: 10px; letter-spacing: 0.08em; text-transform: uppercase; color: var(--ink); }
.fam-count { font-size: 11px; color: var(--k); }
.fam-tag { font-size: 9px; color: var(--ink-dim); line-height: 1.2; }

.workbench {
  display: grid;
  grid-template-columns: 250px 1fr;
  gap: 14px;
  flex: 1;
  min-height: 0;
}
.workbench > .rail { max-height: calc(100vh - 260px); }
.results { display: flex; flex-direction: column; min-height: 0; padding: 12px; }
.results-head { display: flex; flex-direction: column; gap: 8px; margin-bottom: 8px; }
.chips { display: flex; flex-wrap: wrap; gap: 6px; }
.chips .chip { cursor: pointer; }
.err { color: var(--fault); font-size: 12px; }
.pager {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 14px;
  padding-top: 10px;
  border-top: 1px solid var(--line-soft);
  font-size: 11px;
  color: var(--ink-dim);
}

@media (max-width: 1100px) {
  .family-strip { grid-template-columns: repeat(3, 1fr); }
  .workbench { grid-template-columns: 1fr; }
}
</style>
