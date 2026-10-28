<script setup>
// Stats home — the all-families overview behind the Stats tab. Every figure comes from
// src/generated/catalog-summary.json (written at build time by scripts/build-home-summary.mjs
// over the served shards), so this page downloads no shard; a family's own survey (live
// histograms, vendor share, spec coverage) is one click further, in StatsView.
import { computed } from 'vue'
import summary from '../generated/catalog-summary.json'
import { FAMILIES } from '../families.js'
import { store } from '../store.js'
import { pct } from '../units.js'

const rows = computed(() => summary.families
  .map((s) => {
    const f = FAMILIES.find((x) => x.key === s.key)
    if (!f) throw new Error(`catalog summary lists family '${s.key}' that families.js does not define`)
    return { ...s, label: f.label, prodShare: s.production == null ? null : s.production / s.parts }
  })
  .sort((a, b) => b.parts - a.parts))

const maxParts = computed(() => Math.max(...rows.value.map((r) => r.parts)))

// production share is only defined for the families that carry a status flag; the overall
// figure is taken over THOSE families' parts, not the whole catalogue
const flagged = computed(() => rows.value.filter((r) => r.production != null))
const prodShare = computed(() => {
  const parts = flagged.value.reduce((a, r) => a + r.parts, 0)
  return parts ? flagged.value.reduce((a, r) => a + r.production, 0) / parts : null
})
const flaggedParts = computed(() => flagged.value.reduce((a, r) => a + r.parts, 0))
const unreadable = computed(() => rows.value.reduce((a, r) => a + r.unreadableRows, 0))

const fmt = (n) => n.toLocaleString('en-US')

function openFamily(key) {
  store.family = key
  store.statsHome = false
}
</script>

<template>
  <div class="stats-home">
    <div class="tiles">
      <div class="tile panel">
        <span class="tile-label">catalogued parts</span>
        <span class="tile-value mono">{{ fmt(summary.totals.parts) }}</span>
      </div>
      <div class="tile panel">
        <span class="tile-label">families</span>
        <span class="tile-value mono">{{ summary.totals.families }}</span>
      </div>
      <div class="tile panel">
        <span class="tile-label">manufacturer names</span>
        <span class="tile-value mono">{{ summary.totals.manufacturerNames }}</span>
      </div>
      <div v-if="prodShare != null" class="tile panel">
        <span class="tile-label">in production <span class="dim">({{ fmt(flaggedParts) }} parts carry a status)</span></span>
        <span class="tile-value mono">{{ pct(prodShare, 1) }}</span>
      </div>
      <div class="tile panel">
        <span class="tile-label">unreadable source rows</span>
        <span class="tile-value mono">{{ fmt(unreadable) }}</span>
      </div>
    </div>

    <section class="panel pane">
      <p class="section-label">Parts per family <span class="dim">— click a family for its full survey</span></p>
      <button
        v-for="r in rows" :key="r.key"
        class="bar-row" type="button"
        @click="openFamily(r.key)"
      >
        <span class="bar-label">{{ r.label }}</span>
        <span class="bar-track"><i :style="{ width: pct(r.parts / maxParts) }" /></span>
        <span class="bar-n mono">{{ fmt(r.parts) }}</span>
      </button>
    </section>

    <section class="panel pane">
      <p class="section-label">By family</p>
      <table class="fam-table">
        <thead>
          <tr>
            <th>Family</th>
            <th class="num">Parts</th>
            <th class="num">Manufacturers</th>
            <th class="num">In production</th>
            <th class="num">Unreadable rows</th>
            <th>Largest manufacturers</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="r in rows" :key="r.key">
            <td><a :href="`#/stats/${r.key}`" @click.prevent="openFamily(r.key)">{{ r.label }}</a></td>
            <td class="num mono">{{ fmt(r.parts) }}</td>
            <td class="num mono">{{ r.manufacturers }}</td>
            <td class="num mono">{{ r.prodShare == null ? '—' : pct(r.prodShare, 1) }}</td>
            <td class="num mono">{{ fmt(r.unreadableRows) }}</td>
            <td class="dim-cell">{{ r.topManufacturers.map((m) => m.name).join(' · ') }}</td>
          </tr>
        </tbody>
      </table>
      <p class="note">
        Figures are counted over the shipped index at build time. How well the data checks out
        (provenance, physics, fabrication gates) is on the
        <a href="/quality">data quality page</a>.
      </p>
    </section>
  </div>
</template>

<style scoped>
.stats-home { display: flex; flex-direction: column; gap: 14px; flex: 1; min-height: 0; overflow-y: auto; }
.tiles { display: flex; flex-wrap: wrap; gap: 10px; }
.tile { padding: 12px 18px; display: flex; flex-direction: column; gap: 4px; }
.tile-label { font-family: var(--disp); font-size: 9px; letter-spacing: 0.18em; text-transform: uppercase; color: var(--ink-dim); }
.tile-value { font-size: 22px; color: var(--k-hi); }
.pane { padding: 16px; }
.dim { text-transform: none; letter-spacing: 0; color: #4a5b6e; }

.bar-row {
  display: grid; grid-template-columns: 150px 1fr 76px; gap: 10px; align-items: center;
  width: 100%; padding: 3px 0; font-size: 11px;
  background: none; border: 0; text-align: left; cursor: pointer; color: inherit;
}
.bar-row:hover .bar-label { color: var(--k-hi); }
.bar-label { color: var(--ink); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bar-track { height: 12px; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 3px; overflow: hidden; }
.bar-track i { display: block; height: 100%; background: var(--s1); border-radius: 2px 0 0 2px; }
.bar-n { text-align: right; color: var(--ink-dim); font-size: 10px; }

.fam-table { width: 100%; border-collapse: collapse; font-size: 12px; }
.fam-table th { text-align: left; font-family: var(--disp); font-size: 9px; letter-spacing: 0.14em; text-transform: uppercase; color: var(--ink-dim); padding: 4px 10px 6px 0; }
.fam-table td { padding: 4px 10px 4px 0; border-top: 1px solid var(--line-soft); }
.fam-table .num { text-align: right; }
.dim-cell { color: var(--ink-dim); font-size: 11px; }
.note { font-size: 11px; color: var(--ink-dim); margin: 10px 0 0; }
</style>
