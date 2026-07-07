<script setup>
// Left filter rail: unit-aware min/max inputs per numeric spec + faceted value
// lists with live counts (zero-count values grey out, never vanish) + MPN search.
import { computed, reactive, ref, watch } from 'vue'
import { parseSI, si } from '../units.js'

const props = defineProps({
  family: { type: Object, required: true }, // families.js entry
  facets: { type: Object, default: null },  // browse result .facets
  ranges: { type: Object, default: null },  // browse result .ranges
  modelValue: { type: Object, required: true }, // {mpn, num:{f:{min,max}}, sel:{field:[...]}}
})
const emit = defineEmits(['update:modelValue'])

// local text state for numeric inputs (so "4u7" stays as typed while parsed upstream)
const text = reactive({})
watch(
  () => props.family.key,
  () => { for (const k of Object.keys(text)) delete text[k] },
)

function commitNum(f, side, raw) {
  let v = parseSI(raw)
  // scaled display columns (tol %, ppm): the user types display units, the shard
  // stores the raw fraction — divide back before filtering
  const col = props.family.columns.find((c) => c.f === f)
  if (v != null && !Number.isNaN(v) && col?.scale) v = v / col.scale
  const next = JSON.parse(JSON.stringify(props.modelValue))
  next.num[f] = next.num[f] ?? {}
  if (v == null) delete next.num[f][side]
  else if (!Number.isNaN(v)) next.num[f][side] = v
  if (next.num[f] && Object.keys(next.num[f]).length === 0) delete next.num[f]
  emit('update:modelValue', next)
}

function invalid(f, side) {
  const raw = text[`${f}.${side}`]
  if (raw == null || raw.trim() === '') return false
  return Number.isNaN(parseSI(raw))
}

function toggleFacet(field, value) {
  const next = JSON.parse(JSON.stringify(props.modelValue))
  const cur = new Set(next.sel[field] ?? [])
  cur.has(value) ? cur.delete(value) : cur.add(value)
  if (cur.size) next.sel[field] = [...cur]
  else delete next.sel[field]
  emit('update:modelValue', next)
}

function setMpn(v) {
  emit('update:modelValue', { ...JSON.parse(JSON.stringify(props.modelValue)), mpn: v })
}

const expanded = reactive({})
const FACET_PREVIEW = 8

function facetValues(field) {
  const f = props.facets?.[field]
  if (!f) return { values: [], omitted: 0 }
  const selected = new Set(props.modelValue.sel[field] ?? [])
  const values = expanded[field] ? f.values : f.values.slice(0, FACET_PREVIEW)
  // keep selected values visible even when collapsed
  const shown = new Set(values.map((v) => v[0]))
  const extra = f.values.filter((v) => selected.has(v[0]) && !shown.has(v[0]))
  return { values: [...values, ...extra], omitted: f.omitted, more: f.values.length - values.length + f.omitted }
}

const rangeHint = (f) => {
  const r = props.ranges?.[f]
  if (!r || r.present === 0) return ''
  const col = props.family.columns.find((c) => c.f === f)
  const scale = col?.scale ?? 1
  const fmt = (v) => col?.plain
    ? `${Number((v * scale).toPrecision(3))}${col.unit ? ` ${col.unit}` : ''}`
    : si(v * scale, col?.unit ?? '', 2)
  return `${fmt(r.min)} … ${fmt(r.max)}`
}

const numericFields = computed(() => props.family.columns.filter((c) => !c.str && !c.bool))
</script>

<template>
  <aside class="rail panel">
    <p class="section-label">Filter</p>

    <label class="mpn-search">
      <span class="f-label">MPN contains</span>
      <input
        :value="modelValue.mpn"
        type="search"
        placeholder="search part number"
        @input="setMpn($event.target.value)"
      />
    </label>

    <div v-for="c in numericFields" :key="c.f" class="num-filter">
      <span class="f-label">{{ c.label }} <template v-if="c.unit">({{ c.unit }})</template></span>
      <div class="minmax">
        <input
          v-model="text[`${c.f}.min`]"
          :class="{ bad: invalid(c.f, 'min') }"
          placeholder="min"
          @change="commitNum(c.f, 'min', text[`${c.f}.min`])"
          @keyup.enter="commitNum(c.f, 'min', text[`${c.f}.min`])"
        />
        <span class="dash">–</span>
        <input
          v-model="text[`${c.f}.max`]"
          :class="{ bad: invalid(c.f, 'max') }"
          placeholder="max"
          @change="commitNum(c.f, 'max', text[`${c.f}.max`])"
          @keyup.enter="commitNum(c.f, 'max', text[`${c.f}.max`])"
        />
      </div>
      <span class="hint mono">{{ rangeHint(c.f) }}</span>
    </div>

    <div v-for="facet in [{ f: 'manufacturer', label: 'Manufacturer' }, ...family.facets]" :key="facet.f" class="facet">
      <span class="f-label">{{ facet.label }}</span>
      <ul>
        <li v-for="[value, count] in facetValues(facet.f).values" :key="value">
          <label :class="{ zero: count === 0 }">
            <input
              type="checkbox"
              :checked="(modelValue.sel[facet.f] ?? []).includes(value)"
              @change="toggleFacet(facet.f, value)"
            />
            <span class="v">{{ value || '—' }}</span>
            <span class="count mono">{{ count }}</span>
          </label>
        </li>
      </ul>
      <button
        v-if="facetValues(facet.f).more > 0"
        class="more"
        type="button"
        @click="expanded[facet.f] = !expanded[facet.f]"
      >
        {{ expanded[facet.f] ? 'show fewer' : `show ${facetValues(facet.f).more} more` }}
      </button>
      <span v-if="expanded[facet.f] && facetValues(facet.f).omitted > 0" class="hint">
        {{ facetValues(facet.f).omitted }} rarer values not listed
      </span>
    </div>
  </aside>
</template>

<style scoped>
.rail { padding: 14px; display: flex; flex-direction: column; gap: 14px; overflow-y: auto; }
.f-label { display: block; font-size: 11px; color: var(--ink-dim); margin-bottom: 4px; }
.mpn-search input { width: 100%; }
.minmax { display: flex; align-items: center; gap: 6px; }
.minmax input { width: 100%; min-width: 0; }
.minmax input.bad { border-color: var(--fault); }
.dash { color: var(--ink-dim); }
.hint { display: block; font-size: 10px; color: #4a5b6e; margin-top: 3px; }
.facet ul { list-style: none; margin: 0; padding: 0; max-height: 300px; overflow-y: auto; }
.facet label {
  display: flex;
  align-items: center;
  gap: 7px;
  padding: 2px 0;
  font-size: 12px;
  cursor: pointer;
}
.facet label.zero { opacity: 0.35; }
.facet .v { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.facet .count { color: var(--ink-dim); font-size: 10px; }
.facet input[type='checkbox'] { accent-color: var(--k); }
.more { margin-top: 4px; font-size: 10px; padding: 2px 8px; }
</style>
