<script setup>
// Part drawer: the full datasheet record (one Range slice) — headline specs, every
// chartable curve the datasheet carries, the flattened spec sheet, and provenance.
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { record, store, togglePin, isPinned, pinColor } from '../store.js'
import { extractCurves, specRows, familyNode, unitFor } from '../curves.js'
import { familyByKey } from '../families.js'
import { si } from '../units.js'
import CurveChart from './CurveChart.vue'

const rec = ref(null)
const err = ref('')
const loading = ref(false)

const part = computed(() => store.drawer)
const fam = computed(() => familyByKey(part.value?.family))

watch(part, async (p) => {
  rec.value = null
  err.value = ''
  if (!p) return
  loading.value = true
  try {
    rec.value = await record(p.family, p.srcOffset, p.srcLength)
  } catch (e) {
    err.value = e.message
  } finally {
    loading.value = false
  }
}, { immediate: true })

const info = computed(() => familyNode(rec.value)?.manufacturerInfo ?? null)
const curves = computed(() => (rec.value ? extractCurves(rec.value) : []))
const specs = computed(() => (rec.value ? specRows(rec.value) : []))
const groups = computed(() => {
  const g = new Map()
  for (const row of specs.value) {
    if (!g.has(row.group)) g.set(row.group, [])
    g.get(row.group).push(row)
  }
  return [...g.entries()]
})

// headline: the family's own columns, from the shard row we already have
const headline = computed(() => {
  if (!part.value || !fam.value) return []
  return fam.value.columns
    .filter((c) => part.value[c.f] != null)
    .map((c) => ({
      label: c.label,
      value: c.str || c.bool
        ? String(part.value[c.f])
        : si(part.value[c.f] * (c.scale ?? 1), c.unit),
    }))
})

function fmtSpec(row) {
  if (typeof row.value === 'number') {
    const pretty = Math.abs(row.value) >= 1e-15 && Math.abs(row.value) < 1e15
      ? si(row.value, unitFor(row.key), 4)
      : String(row.value)
    return row.dim ? `${pretty} (nom)` : pretty
  }
  return String(row.value)
}

function close() {
  store.drawer = null
}
function onKey(e) {
  if (e.key === 'Escape') close()
}
onMounted(() => window.addEventListener('keydown', onKey))
onUnmounted(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <Teleport to="body">
    <div v-if="part" class="backdrop" @click.self="close">
      <aside class="drawer panel" data-testid="part-drawer">
        <header class="drawer-head">
          <div>
            <h2 class="mono">{{ part.mpn }}</h2>
            <p class="sub">
              {{ part.manufacturer }}
              <span v-if="info?.status" class="chip" :class="{ on: info.status === 'production' }">{{ info.status }}</span>
              <span v-if="info?.family" class="series">{{ info.family }}</span>
            </p>
          </div>
          <div class="head-actions">
            <button
              type="button"
              :style="isPinned(part.mpn) ? { color: pinColor(part.mpn), borderColor: pinColor(part.mpn) } : {}"
              @click="togglePin(part.family, part)"
            >
              {{ isPinned(part.mpn) ? '◉ pinned' : '◉ pin to compare' }}
            </button>
            <button type="button" @click="close">✕</button>
          </div>
        </header>

        <div class="drawer-body">
          <div v-if="headline.length" class="kv-strip">
            <div v-for="h in headline" :key="h.label" class="kv">
              <span class="kv-label">{{ h.label }}</span>
              <span class="kv-value mono">{{ h.value }}</span>
            </div>
          </div>

          <p v-if="loading" class="note">Reading record from the catalog… (one Range slice)</p>
          <p v-if="err" class="err mono">{{ err }}</p>

          <template v-if="rec">
            <section v-if="curves.length">
              <p class="section-label">Datasheet curves</p>
              <CurveChart v-for="c in curves" :key="c.key + c.title" :curve="c" :height="230" />
            </section>
            <p v-else class="note">This record carries no curve data — scalar specs only.</p>

            <section v-for="[group, rows] in groups" :key="group">
              <p class="section-label">{{ group }}</p>
              <table class="spec-table">
                <tbody>
                  <tr v-for="row in rows" :key="group + row.key">
                    <td class="spec-key">{{ row.key }}</td>
                    <td class="spec-val mono">{{ fmtSpec(row) }}</td>
                  </tr>
                </tbody>
              </table>
            </section>

            <section v-if="info?.datasheetUrl">
              <p class="section-label">Source</p>
              <a :href="info.datasheetUrl" target="_blank" rel="noopener">manufacturer datasheet ↗</a>
            </section>
          </template>
        </div>
      </aside>
    </div>
  </Teleport>
</template>

<style scoped>
.backdrop {
  position: fixed;
  inset: 0;
  background: rgba(4, 7, 11, 0.6);
  backdrop-filter: blur(2px);
  z-index: 40;
}
.drawer {
  position: absolute;
  top: 0;
  right: 0;
  bottom: 0;
  width: min(620px, 94vw);
  border-radius: 0;
  border-right: none;
  display: flex;
  flex-direction: column;
}
.drawer-head {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 10px;
  padding: 16px;
  border-bottom: 1px solid var(--line);
}
h2 { margin: 0; font-size: 17px; color: var(--k-hi); }
.sub { margin: 4px 0 0; color: var(--ink-dim); font-size: 12px; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.series { font-size: 11px; }
.head-actions { display: flex; gap: 8px; flex-shrink: 0; }
.drawer-body { padding: 16px; overflow-y: auto; display: flex; flex-direction: column; gap: 18px; }
.kv-strip { display: flex; flex-wrap: wrap; gap: 10px; }
.kv {
  background: var(--bg-deep);
  border: 1px solid var(--line-soft);
  border-radius: 4px;
  padding: 6px 12px;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.kv-label { font-size: 9px; letter-spacing: 0.1em; text-transform: uppercase; color: var(--ink-dim); }
.kv-value { font-size: 13px; color: var(--k-hi); }
.note { color: var(--ink-dim); font-size: 12px; }
.err { color: var(--fault); font-size: 12px; }
.spec-table { width: 100%; border-collapse: collapse; font-size: 12px; }
.spec-table td { padding: 4px 8px; border-bottom: 1px solid var(--line-soft); }
.spec-key { color: var(--ink-dim); }
.spec-val { text-align: right; }
</style>
