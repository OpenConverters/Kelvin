<script setup>
// KELVIN — deterministic parts librarian. The shell: instrument-faceplate header
// with a calibration block, the temperature rail (amber→ice; it fills as the index
// cools to operating temperature), mode tabs, views, compare tray, part drawer.
import { computed, onMounted, reactive, ref } from 'vue'
import { loadEngine, manifest, ensureShard, shardEvents } from './engine.js'
import { FAMILIES } from './families.js'
import { store, syncUrl, restoreFromUrl, bindHashNavigation, clearPins } from './store.js'
import CatalogView from './views/CatalogView.vue'
import RecommendView from './views/RecommendView.vue'
import CompareView from './views/CompareView.vue'
import PartDrawer from './components/PartDrawer.vue'

const boot = reactive({ phase: 'cold', detail: 'powering engine…', error: '' })
const counts = ref({})
const totalParts = computed(() => Object.values(counts.value).reduce((a, b) => a + b, 0))
const buildTag = ref('')

const shardState = reactive({ loading: new Set(), loaded: new Set() })
shardEvents.addEventListener('shard', (ev) => {
  const { family, phase } = ev.detail
  if (phase === 'loading') shardState.loading.add(family)
  else {
    shardState.loading.delete(family)
    if (phase === 'loaded') shardState.loaded.add(family)
  }
})

// rail: 0→35% engine boot, →55% manifest, then toward 100% with loaded shards
const railPct = computed(() => {
  if (boot.phase === 'cold') return 8
  if (boot.phase === 'engine') return 35
  const n = FAMILIES.length
  return Math.round(55 + (shardState.loaded.size / n) * 45)
})
const railLabel = computed(() => {
  if (boot.error) return 'FAULT'
  if (boot.phase !== 'ready') return boot.detail
  if (shardState.loading.size) return `cooling ${[...shardState.loading].join(', ')} shard…`
  return `${shardState.loaded.size}/${FAMILIES.length} shards at operating temperature`
})

onMounted(async () => {
  const familyKeys = FAMILIES.map((f) => f.key)
  restoreFromUrl(familyKeys)
  bindHashNavigation(familyKeys)
  syncUrl()
  try {
    await loadEngine()
    boot.phase = 'engine'
    boot.detail = 'reading calibration manifest…'
    const m = await manifest()
    const c = {}
    for (const [fam, e] of Object.entries(m.families)) c[fam] = e.rows ?? 0
    counts.value = c
    // calibration tag: first 6 hex-ish chars of the magnetic (largest churn) buildId, or any
    const anyBuild = m.families[store.family]?.buildId ?? Object.values(m.families)[0]?.buildId ?? ''
    buildTag.value = String(anyBuild).slice(0, 8)
    boot.phase = 'ready'
    boot.detail = ''
    ensureShard(store.family) // pre-cool the active family
  } catch (e) {
    boot.error = e.message
    boot.detail = ''
  }
})

const VIEWS = [
  { key: 'catalog', label: 'Catalog' },
  { key: 'recommend', label: 'Recommend' },
  { key: 'compare', label: 'Compare' },
]
</script>

<template>
  <div class="wrap">
    <header class="kv-head panel">
      <div class="brand">
        <svg class="logo" viewBox="0 0 64 64" aria-hidden="true">
          <rect x="14" y="28" width="36" height="8" rx="2" fill="none" stroke="currentColor" stroke-width="2.4" />
          <path d="M14 32H4M60 32H50" stroke="currentColor" stroke-width="2.4" fill="none" />
          <path d="M22 28V10M42 36v18" stroke="currentColor" stroke-width="1.6" fill="none" />
          <circle cx="22" cy="10" r="3" fill="currentColor" />
          <circle cx="42" cy="54" r="3" fill="currentColor" />
          <circle cx="22" cy="28" r="2.2" fill="currentColor" />
          <circle cx="42" cy="36" r="2.2" fill="currentColor" />
        </svg>
        <div class="brand-text">
          <h1>KELVIN</h1>
          <p>deterministic parts librarian</p>
        </div>
      </div>

      <nav class="modes" aria-label="mode">
        <button
          v-for="v in VIEWS" :key="v.key"
          class="mode" :class="{ active: store.view === v.key }"
          type="button"
          @click="store.view = v.key"
        >
          {{ v.label }}
          <span v-if="v.key === 'compare' && store.pins.length" class="pin-count mono">{{ store.pins.length }}</span>
        </button>
      </nav>

      <!-- calibration block: the metrology sticker -->
      <dl class="cal mono" aria-label="index calibration">
        <div><dt>PARTS</dt><dd>{{ totalParts ? totalParts.toLocaleString() : '—' }}</dd></div>
        <div><dt>FAMILIES</dt><dd>{{ Object.keys(counts).length || '—' }}</dd></div>
        <div><dt>INDEX</dt><dd>{{ buildTag || '—' }}</dd></div>
        <div class="led-cell">
          <dt>STATUS</dt>
          <dd><i class="led" :class="boot.error ? 'fault' : boot.phase === 'ready' ? 'ok' : 'warm'" />{{ boot.error ? 'FAULT' : boot.phase === 'ready' ? 'READY' : 'BOOT' }}</dd>
        </div>
      </dl>
    </header>

    <!-- the temperature rail: 2 000 K amber → 10 000 K ice; fills as the index cools -->
    <div class="rail-line" role="progressbar" :aria-valuenow="railPct" aria-valuemin="0" aria-valuemax="100" :aria-label="railLabel">
      <div class="rail-fill" :style="{ width: railPct + '%' }" />
      <span class="rail-label mono">{{ railLabel }}</span>
    </div>

    <p v-if="boot.error" class="boot-error mono panel">
      Engine fault: {{ boot.error }}
    </p>

    <main v-else class="main">
      <CatalogView v-if="store.view === 'catalog'" :counts="counts" />
      <RecommendView v-else-if="store.view === 'recommend'" />
      <CompareView v-else />
    </main>

    <!-- compare tray -->
    <div v-if="store.pins.length && store.view !== 'compare'" class="tray panel">
      <span class="tray-label section-label">compare tray</span>
      <span v-for="p in store.pins" :key="p.mpn" class="chip on tray-chip">
        <i class="dot" :style="{ background: p.color }" />{{ p.mpn }}
      </span>
      <span v-if="store.pinNote" class="tray-note">{{ store.pinNote }}</span>
      <button class="primary" type="button" @click="store.view = 'compare'">overlay curves ›</button>
      <button type="button" @click="clearPins">clear</button>
    </div>
    <p v-else-if="store.pinNote" class="tray-note-solo mono">{{ store.pinNote }}</p>

    <footer class="foot">
      <span>same question, same answer — selection is deterministic and runs entirely in your browser</span>
      <span class="foot-links">
        sibling of
        <a href="https://kirchhoff.openconverters.com" target="_blank" rel="noopener">Kirchhoff</a> ·
        <a href="https://openmagnetics.com" target="_blank" rel="noopener">OpenMagnetics</a>
      </span>
    </footer>

    <PartDrawer />
  </div>
</template>

<style scoped>
.wrap {
  max-width: 1680px;
  margin: 0 auto;
  padding: 14px 18px 10px;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

/* ── header faceplate ─────────────────────────────────────────────────────── */
.kv-head {
  display: flex;
  align-items: center;
  gap: 26px;
  padding: 12px 18px;
}
.brand { display: flex; align-items: center; gap: 14px; }
.logo { width: 40px; height: 40px; color: var(--k); filter: drop-shadow(0 0 6px rgba(127, 201, 255, 0.45)); }
.brand-text h1 {
  margin: 0;
  font-family: var(--disp);
  font-size: 21px;
  letter-spacing: 0.3em;
  color: var(--k-hi);
  text-shadow: 0 0 12px rgba(127, 201, 255, 0.35);
}
.brand-text p {
  margin: 2px 0 0;
  font-size: 10px;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: var(--ink-dim);
}
.modes { display: flex; gap: 6px; margin-left: 10px; }
.mode {
  font-family: var(--disp);
  font-size: 10px;
  letter-spacing: 0.14em;
  text-transform: uppercase;
  padding: 8px 14px;
  position: relative;
}
.mode.active { border-color: var(--k-deep); color: var(--k-hi); box-shadow: 0 0 12px rgba(127, 201, 255, 0.12) inset; }
.pin-count {
  background: var(--k);
  color: #05121e;
  border-radius: 999px;
  font-size: 9px;
  padding: 0 5px;
  margin-left: 4px;
}
.cal { display: flex; gap: 18px; margin: 0 0 0 auto; }
.cal div { display: flex; flex-direction: column; gap: 1px; }
.cal dt { font-size: 8px; letter-spacing: 0.2em; color: var(--ink-dim); }
.cal dd { margin: 0; font-size: 12px; color: var(--k-hi); display: flex; align-items: center; gap: 6px; }
.led { width: 7px; height: 7px; border-radius: 50%; display: inline-block; }
.led.ok { background: var(--ok); box-shadow: 0 0 6px var(--ok); }
.led.warm { background: var(--warm); box-shadow: 0 0 6px var(--warm); animation: blink 1.1s infinite; }
.led.fault { background: var(--fault); box-shadow: 0 0 6px var(--fault); }
@keyframes blink { 50% { opacity: 0.35; } }

/* ── the temperature rail ─────────────────────────────────────────────────── */
.rail-line {
  position: relative;
  height: 14px;
  border: 1px solid var(--line-soft);
  border-radius: 4px;
  background: var(--bg-deep);
  overflow: hidden;
}
.rail-fill {
  height: 100%;
  background: linear-gradient(90deg, #ff8a3d 0%, var(--warm) 18%, #ffd9a0 38%, #eef4ff 62%, #a8d4ff 82%, var(--k) 100%);
  opacity: 0.85;
  transition: width 0.5s ease-out;
}
.rail-label {
  position: absolute;
  right: 8px;
  top: 0;
  line-height: 14px;
  font-size: 9px;
  letter-spacing: 0.1em;
  color: var(--ink-dim);
  text-shadow: 0 0 4px var(--bg-deep);
}

.boot-error { color: var(--fault); padding: 16px; font-size: 12px; }
.main { flex: 1; min-height: 0; display: flex; flex-direction: column; }

/* ── compare tray ─────────────────────────────────────────────────────────── */
.tray {
  position: sticky;
  bottom: 10px;
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  z-index: 20;
  box-shadow: 0 6px 24px rgba(0, 0, 0, 0.5);
}
.tray-label { margin: 0; }
.tray-chip { max-width: 190px; overflow: hidden; }
.tray-note, .tray-note-solo { font-size: 11px; color: var(--warm); }
.tray button:last-child { margin-left: 0; }
.tray .primary { margin-left: auto; }
.dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; }

.foot {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  font-size: 10px;
  color: #4a5b6e;
  padding: 4px 2px 6px;
}

@media (max-width: 1100px) {
  .kv-head { flex-wrap: wrap; gap: 12px; }
  .cal { margin-left: 0; flex-wrap: wrap; }
  .foot { flex-direction: column; }
}
</style>
