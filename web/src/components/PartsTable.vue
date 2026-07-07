<script setup>
// Results table: sortable spec columns, SI-formatted mono values, pin-to-compare,
// row click opens the part drawer. Pinned rows carry their series color chip —
// the table and the overlay chart describe the same set (chart-linked results).
import { si } from '../units.js'
import { isPinned, pinColor } from '../store.js'

const props = defineProps({
  family: { type: Object, required: true },
  rows: { type: Array, required: true },
  sort: { type: Object, required: true }, // {field, dir}
  busy: { type: Boolean, default: false },
})
const emit = defineEmits(['sort', 'open', 'pin'])

function clickSort(f) {
  if (props.sort.field === f) {
    emit('sort', { field: f, dir: props.sort.dir === 'asc' ? 'desc' : 'asc' })
  } else {
    emit('sort', { field: f, dir: 'asc' })
  }
}

function cell(row, col) {
  const v = row[col.f]
  if (v == null) return '—'
  if (col.bool) return v ? '●' : '—'
  if (col.str) return v
  const scaled = (col.scale ?? 1) * v
  return col.plain ? `${Math.round(scaled * 100) / 100}${col.unit ? ` ${col.unit}` : ''}` : si(scaled, col.unit)
}

function sortClass(f) {
  return props.sort.field === f ? `sorted-${props.sort.dir}` : ''
}
</script>

<template>
  <div class="table-wrap" :class="{ busy }">
    <table>
      <thead>
        <tr>
          <th class="pin-col" aria-label="pin"></th>
          <th class="sortable" :class="sortClass('mpn')" @click="clickSort('mpn')">MPN</th>
          <th class="sortable" :class="sortClass('manufacturer')" @click="clickSort('manufacturer')">Manufacturer</th>
          <th
            v-for="c in family.columns" :key="c.f"
            class="num sortable" :class="sortClass(c.f)"
            @click="clickSort(c.f)"
          >{{ c.label }}</th>
        </tr>
      </thead>
      <tbody>
        <tr
          v-for="row in rows"
          :key="row.lineno"
          :class="{ pinned: isPinned(row.mpn) }"
          @click="emit('open', row)"
        >
          <td class="pin-col" @click.stop="emit('pin', row)">
            <button
              class="pin-btn"
              type="button"
              :title="isPinned(row.mpn) ? 'unpin from compare' : 'pin to compare'"
              :style="isPinned(row.mpn) ? { color: pinColor(row.mpn), borderColor: pinColor(row.mpn) } : {}"
            >◉</button>
          </td>
          <td class="mono mpn">{{ row.mpn }}</td>
          <td class="mfr">{{ row.manufacturer }}</td>
          <td v-for="c in family.columns" :key="c.f" class="num mono">{{ cell(row, c) }}</td>
        </tr>
        <tr v-if="!rows.length && !busy">
          <td :colspan="family.columns.length + 3" class="empty">
            No parts match — widen a filter or clear one from the chips above.
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<style scoped>
.table-wrap { overflow: auto; flex: 1; min-height: 0; }
.table-wrap.busy { opacity: 0.55; pointer-events: none; }
table { width: 100%; border-collapse: collapse; font-size: 12px; }
thead th {
  position: sticky;
  top: 0;
  z-index: 1;
  background: var(--panel-hi);
  border-bottom: 1px solid var(--line);
  padding: 8px 10px;
  text-align: left;
  font-family: var(--disp);
  font-size: 9px;
  letter-spacing: 0.14em;
  text-transform: uppercase;
  color: var(--ink-dim);
  white-space: nowrap;
}
th.num, td.num { text-align: right; }
th.sortable { cursor: pointer; user-select: none; }
th.sortable:hover { color: var(--k-hi); }
th.sorted-asc::after { content: ' ▲'; color: var(--k); }
th.sorted-desc::after { content: ' ▼'; color: var(--k); }
tbody tr { border-bottom: 1px solid var(--line-soft); cursor: pointer; }
tbody tr:hover { background: rgba(127, 201, 255, 0.05); }
tbody tr.pinned { background: rgba(127, 201, 255, 0.07); }
td { padding: 6px 10px; white-space: nowrap; }
td.mpn { color: var(--k-hi); }
td.mfr { color: var(--ink-dim); max-width: 180px; overflow: hidden; text-overflow: ellipsis; }
.pin-col { width: 34px; padding: 4px 6px; }
.pin-btn { padding: 1px 6px; font-size: 12px; color: var(--ink-dim); border-radius: 999px; }
.empty { text-align: center; color: var(--ink-dim); padding: 30px; }
</style>
