<script setup>
// Null-balance margin meter — the Kelvin-bridge signature. Each requirement is a
// galvanometer strip: the needle at null means the part exactly meets the spec;
// deflection right = headroom, left = shortfall. Ranking stays auditable because
// every criterion shows its own needle.
import { computed } from 'vue'
import { pct } from '../units.js'

const props = defineProps({
  label: { type: String, required: true },
  value: { type: Number, default: null }, // headroom fraction (0 = exact) or ratio (1 = exact)
  kind: { type: String, default: 'headroom' }, // 'headroom' | 'ratio'
  verdict: { type: String, default: null }, // optional pass|marginal|fail override
})

// deflection in [-1, 1]; ratio meters deflect by log10 (a ×10 miss = full scale)
const deflection = computed(() => {
  if (props.value == null || Number.isNaN(props.value)) return null
  const d = props.kind === 'ratio'
    ? (props.value > 0 ? Math.log10(props.value) : -1)
    : props.value
  return Math.max(-1, Math.min(1, d))
})

const state = computed(() => {
  if (props.verdict) return props.verdict
  if (deflection.value == null) return 'unknown'
  if (deflection.value < (props.kind === 'ratio' ? -0.02 : -1e-9)) return 'fail'
  if (props.kind === 'ratio' && Math.abs(deflection.value) > 0.3) return 'marginal'
  return 'pass'
})

const display = computed(() => {
  if (props.value == null || Number.isNaN(props.value)) return 'n/a'
  if (props.kind === 'ratio') return `×${props.value.toPrecision(3)}`
  return (props.value >= 0 ? '+' : '') + pct(props.value)
})
</script>

<template>
  <div class="meter" :class="state">
    <span class="meter-label">{{ label }}</span>
    <span class="track" aria-hidden="true">
      <i class="null-mark" />
      <i v-if="deflection != null" class="needle" :style="{ left: `${50 + deflection * 48}%` }" />
    </span>
    <span class="meter-value mono">{{ display }}</span>
  </div>
</template>

<style scoped>
.meter {
  display: grid;
  grid-template-columns: minmax(90px, 1fr) 120px 64px;
  gap: 8px;
  align-items: center;
  font-size: 11px;
  padding: 2px 0;
}
.meter-label { color: var(--ink-dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.track {
  position: relative;
  height: 12px;
  background: var(--bg-deep);
  border: 1px solid var(--line-soft);
  border-radius: 3px;
  background-image: linear-gradient(90deg, transparent 24%, var(--grat-strong) 24.5%, transparent 25%, transparent 74%, var(--grat-strong) 74.5%, transparent 75%);
}
.null-mark {
  position: absolute;
  left: 50%;
  top: -2px;
  bottom: -2px;
  width: 1px;
  background: var(--ink-dim);
  opacity: 0.7;
}
.needle {
  position: absolute;
  top: 1px;
  bottom: 1px;
  width: 2px;
  margin-left: -1px;
  border-radius: 1px;
  transition: left 0.25s ease-out;
}
.meter-value { text-align: right; color: var(--ink); }
.pass .needle { background: var(--ok); box-shadow: 0 0 5px var(--ok); }
.marginal .needle { background: var(--warm); box-shadow: 0 0 5px var(--warm); }
.fail .needle { background: var(--fault); box-shadow: 0 0 5px var(--fault); }
.fail .meter-value { color: var(--fault); }
.unknown .meter-value { color: var(--ink-dim); }
</style>
