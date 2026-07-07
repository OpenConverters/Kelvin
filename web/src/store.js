// App state (no router, no pinia — the family pattern is a single reactive module).
import { reactive, watch } from 'vue'
import { fetchRecord } from './engine.js'

// Validated categorical series palette (dataviz six-checks vs the panel surface).
// Fixed order; a pinned part keeps its color for the life of the pin.
export const KSERIES = ['#3b8fdb', '#c77b28', '#26a0b5', '#b08d22', '#7d74d6', '#c25580']
export const MAX_PINS = KSERIES.length

export const store = reactive({
  view: 'catalog', // catalog | recommend | compare
  family: 'magnetic',
  pins: [], // [{family, mpn, manufacturer, srcOffset, srcLength, color}]
  pinNote: '',
  drawer: null, // {family, mpn, manufacturer, srcOffset, srcLength}
})

export function isPinned(mpn) {
  return store.pins.some((p) => p.mpn === mpn)
}

export function pinColor(mpn) {
  return store.pins.find((p) => p.mpn === mpn)?.color ?? null
}

// Pins live within ONE family at a time (curves are only comparable within a family).
export function togglePin(family, part) {
  store.pinNote = ''
  const i = store.pins.findIndex((p) => p.mpn === part.mpn)
  if (i >= 0) {
    store.pins.splice(i, 1)
    return
  }
  if (store.pins.length && store.pins[0].family !== family) {
    store.pins.splice(0)
    store.pinNote = 'Compare tray cleared — pins compare within one family.'
  }
  if (store.pins.length >= MAX_PINS) {
    store.pinNote = `Compare tray holds ${MAX_PINS} parts — unpin one first.`
    return
  }
  const used = new Set(store.pins.map((p) => p.color))
  const color = KSERIES.find((c) => !used.has(c))
  store.pins.push({
    family,
    mpn: part.mpn,
    manufacturer: part.manufacturer,
    srcOffset: part.srcOffset,
    srcLength: part.srcLength,
    color,
  })
}

export function clearPins() {
  store.pins.splice(0)
  store.pinNote = ''
}

// ── record cache (Range slices are small; keep the session's reads) ───────────
const _records = new Map() // `${family}:${srcOffset}` -> Promise<record>
export function record(family, srcOffset, srcLength) {
  const key = `${family}:${srcOffset}`
  if (!_records.has(key)) {
    _records.set(key, fetchRecord(family, srcOffset, srcLength).catch((e) => {
      _records.delete(key)
      throw e
    }))
  }
  return _records.get(key)
}

// ── URL state: #/<view>/<family> (shareable mode+family; filters stay in-page) ─
export function syncUrl() {
  watch(
    () => [store.view, store.family],
    ([view, family]) => {
      const h = `#/${view}/${family}`
      if (location.hash !== h) history.replaceState(null, '', h)
    },
    { immediate: true },
  )
}

export function restoreFromUrl(validFamilies) {
  const m = location.hash.match(/^#\/(catalog|recommend|compare)\/([a-z]+)$/)
  if (!m) return
  store.view = m[1]
  if (validFamilies.includes(m[2])) store.family = m[2]
}

// Back/forward + pasted #-links switch views without a reload. syncUrl writes via
// replaceState (which never fires hashchange), so this can't loop.
export function bindHashNavigation(validFamilies) {
  window.addEventListener('hashchange', () => restoreFromUrl(validFamilies))
}
