<script setup>
// Datasheet curve plot — hand-rolled SVG (the family pattern: no chart library),
// log/lin axes, decade graticule, multi-series overlay with legend toggles and a
// snapped crosshair readout. Series colors are the validated categorical set
// (fixed order, color follows the entity), passed in by the parent.
import { computed, ref } from 'vue'
import { si } from '../units.js'

const props = defineProps({
  curve: { type: Object, required: true }, // {title,xLabel,xUnit,yLabel,yUnit,xLog,yLog,marks,series:[{name,points,color}]}
  height: { type: Number, default: 260 },
})

const W = 760
const PAD = { l: 64, r: 16, t: 14, b: 34 }

const hidden = ref(new Set())
function toggle(i) {
  const s = new Set(hidden.value)
  s.has(i) ? s.delete(i) : s.add(i)
  hidden.value = s
}

const visSeries = computed(() =>
  props.curve.series.map((s, i) => ({ ...s, i })).filter((s) => !hidden.value.has(s.i)))

const domain = computed(() => {
  let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity
  for (const s of visSeries.value) {
    for (const [x, y] of s.points) {
      if (props.curve.xLog && x <= 0) continue
      if (props.curve.yLog && y <= 0) continue
      if (x < x0) x0 = x
      if (x > x1) x1 = x
      if (y < y0) y0 = y
      if (y > y1) y1 = y
    }
  }
  if (!Number.isFinite(x0)) return null
  if (x0 === x1) { x0 -= 1; x1 += 1 }
  if (y0 === y1) { y0 = y0 === 0 ? -1 : y0 * 0.9; y1 = y1 === 0 ? 1 : y1 * 1.1 }
  if (!props.curve.yLog) { const m = (y1 - y0) * 0.08; y0 -= m; y1 += m }
  if (!props.curve.xLog && !props.curve.marks) { const m = (x1 - x0) * 0.02; x0 -= m; x1 += m }
  return { x0, x1, y0, y1 }
})

function sx(x) {
  const d = domain.value
  const t = props.curve.xLog
    ? (Math.log10(x) - Math.log10(d.x0)) / (Math.log10(d.x1) - Math.log10(d.x0))
    : (x - d.x0) / (d.x1 - d.x0)
  return PAD.l + t * (W - PAD.l - PAD.r)
}
function sy(y) {
  const d = domain.value
  const t = props.curve.yLog
    ? (Math.log10(y) - Math.log10(d.y0)) / (Math.log10(d.y1) - Math.log10(d.y0))
    : (y - d.y0) / (d.y1 - d.y0)
  return props.height - PAD.b - t * (props.height - PAD.t - PAD.b)
}

function ticksFor(lo, hi, log) {
  if (log) {
    const out = []
    const e0 = Math.floor(Math.log10(lo)), e1 = Math.ceil(Math.log10(hi))
    for (let e = e0; e <= e1; e++) {
      const v = Math.pow(10, e)
      if (v >= lo * 0.999 && v <= hi * 1.001) out.push(v)
    }
    if (out.length >= 2) return out
    // less than a decade: fall back to linear ticks inside the log window
  }
  const out = []
  const span = hi - lo
  const step = Math.pow(10, Math.floor(Math.log10(span / 4)))
  const mult = span / step > 8 ? 2.5 : span / step > 4 ? 2 : 1
  const s = step * mult
  for (let v = Math.ceil(lo / s) * s; v <= hi * 1.0001; v += s) out.push(v)
  return out
}

const xTicks = computed(() => (domain.value ? ticksFor(domain.value.x0, domain.value.x1, props.curve.xLog) : []))
const yTicks = computed(() => (domain.value ? ticksFor(domain.value.y0, domain.value.y1, props.curve.yLog) : []))

function pathFor(points) {
  const d = domain.value
  let out = ''
  for (const [x, y] of points) {
    if (props.curve.xLog && x <= 0) continue
    if (props.curve.yLog && y <= 0) continue
    out += `${out ? 'L' : 'M'}${sx(x).toFixed(1)} ${sy(y).toFixed(1)}`
  }
  return out
}

// ── crosshair: snap to nearest data x per visible series ─────────────────────
const hover = ref(null) // {px, rows:[{name,color,x,y}]}
const svgEl = ref(null)

function onMove(ev) {
  const d = domain.value
  if (!d || !svgEl.value) return
  const rect = svgEl.value.getBoundingClientRect()
  const px = ((ev.clientX - rect.left) / rect.width) * W
  if (px < PAD.l || px > W - PAD.r) { hover.value = null; return }
  const t = (px - PAD.l) / (W - PAD.l - PAD.r)
  const xAt = props.curve.xLog
    ? Math.pow(10, Math.log10(d.x0) + t * (Math.log10(d.x1) - Math.log10(d.x0)))
    : d.x0 + t * (d.x1 - d.x0)
  const rows = []
  for (const s of visSeries.value) {
    let best = null, bestDist = Infinity
    for (const [x, y] of s.points) {
      const dist = props.curve.xLog && x > 0 && xAt > 0
        ? Math.abs(Math.log10(x) - Math.log10(xAt))
        : Math.abs(x - xAt)
      if (dist < bestDist) { bestDist = dist; best = [x, y] }
    }
    if (best) rows.push({ name: s.name, color: s.color, x: best[0], y: best[1], i: s.i })
  }
  if (!rows.length) { hover.value = null; return }
  rows.sort((a, b) => b.y - a.y)
  hover.value = { px: sx(rows[0].x), rows }
}
</script>

<template>
  <figure class="curve" role="img" :aria-label="curve.title">
    <figcaption class="curve-title">
      <span>{{ curve.title }}</span>
      <span class="axes-note mono">{{ curve.xLog ? 'log' : 'lin' }}·{{ curve.yLog ? 'log' : 'lin' }}</span>
    </figcaption>
    <svg
      v-if="domain"
      ref="svgEl"
      :viewBox="`0 0 ${W} ${height}`"
      preserveAspectRatio="xMidYMid meet"
      @mousemove="onMove"
      @mouseleave="hover = null"
    >
      <!-- graticule -->
      <g>
        <line
          v-for="v in yTicks" :key="'y' + v"
          :x1="PAD.l" :x2="W - PAD.r" :y1="sy(v)" :y2="sy(v)"
          class="grid"
        />
        <line
          v-for="v in xTicks" :key="'x' + v"
          :y1="PAD.t" :y2="height - PAD.b" :x1="sx(v)" :x2="sx(v)"
          class="grid"
        />
      </g>
      <!-- axes labels -->
      <g class="ticks">
        <text v-for="v in yTicks" :key="'yl' + v" :x="PAD.l - 8" :y="sy(v) + 3" text-anchor="end">
          {{ si(v, curve.yUnit, 2) }}
        </text>
        <text v-for="v in xTicks" :key="'xl' + v" :x="sx(v)" :y="height - PAD.b + 16" text-anchor="middle">
          {{ si(v, curve.xUnit, 2) }}
        </text>
      </g>
      <!-- traces -->
      <g>
        <path
          v-for="s in visSeries" :key="s.i"
          :d="pathFor(s.points)"
          class="trace"
          :stroke="s.color || 'var(--s1)'"
          :class="{ dim: hover && hover.rows.length > 1 }"
        />
        <template v-if="curve.marks">
          <g v-for="s in visSeries" :key="'m' + s.i">
            <circle
              v-for="([x, y], j) in s.points" :key="j"
              :cx="sx(x)" :cy="sy(y)" r="4"
              :fill="s.color || 'var(--s1)'" class="mark"
            />
          </g>
        </template>
      </g>
      <!-- crosshair -->
      <g v-if="hover">
        <line :x1="hover.px" :x2="hover.px" :y1="PAD.t" :y2="height - PAD.b" class="xhair" />
        <circle
          v-for="r in hover.rows" :key="'h' + r.i"
          :cx="sx(r.x)" :cy="sy(r.y)" r="3.5" :fill="r.color || 'var(--s1)'"
          stroke="var(--bg-deep)" stroke-width="1.5"
        />
      </g>
    </svg>
    <p v-else class="curve-empty">No plottable points.</p>

    <!-- readout -->
    <div v-if="hover" class="readout mono">
      <span class="readout-x">{{ curve.xLabel }} = {{ si(hover.rows[0].x, curve.xUnit) }}</span>
      <span v-for="r in hover.rows" :key="r.i" class="readout-row">
        <i class="dot" :style="{ background: r.color || 'var(--s1)' }" />
        <template v-if="r.name">{{ r.name }}: </template>{{ si(r.y, curve.yUnit) }}
      </span>
    </div>

    <!-- legend (toggles) — only for ≥2 series -->
    <div v-if="curve.series.length > 1" class="legend">
      <button
        v-for="(s, i) in curve.series" :key="i"
        class="legend-item" :class="{ off: hidden.has(i) }"
        type="button"
        @click="toggle(i)"
      >
        <i class="dot" :style="{ background: s.color || 'var(--s1)' }" />
        {{ s.name || `series ${i + 1}` }}
      </button>
    </div>
  </figure>
</template>

<style scoped>
.curve { margin: 0 0 14px; }
.curve-title {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  font-size: 12px;
  color: var(--ink);
  margin-bottom: 4px;
}
.axes-note { font-size: 10px; color: var(--ink-dim); }
svg { width: 100%; display: block; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 4px; }
.grid { stroke: var(--grat-strong); stroke-width: 1; }
.ticks text { fill: var(--ink-dim); font-family: var(--mono); font-size: 10px; }
.trace {
  fill: none;
  stroke-width: 2;
  filter: drop-shadow(0 0 3px rgba(127, 201, 255, 0.25));
}
.mark { stroke: var(--bg-deep); stroke-width: 1.5; }
.xhair { stroke: var(--k); stroke-width: 1; stroke-dasharray: 3 3; opacity: 0.8; }
.curve-empty { color: var(--ink-dim); font-size: 12px; }
.readout {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  font-size: 11px;
  color: var(--ink);
  padding: 4px 2px;
}
.readout-x { color: var(--ink-dim); }
.readout-row { display: inline-flex; align-items: center; gap: 5px; }
.legend { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 6px; }
.legend-item {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: 11px;
  padding: 2px 8px;
}
.legend-item.off { opacity: 0.4; }
.dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; }
</style>
