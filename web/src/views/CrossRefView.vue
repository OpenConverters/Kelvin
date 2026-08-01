<script setup>
// Cross-Ref: pick an ORIGINAL catalogue part, mark the manufacturer(s) you want a
// substitute from, and rank that manufacturer's parts with the deterministic
// cross-reference engine (CrossRef.hpp — the same gates + honesty rules Heaviside
// and Kirchhoff consume). Analytical and LLM-free: every verdict is a published
// utility curve over the two datasheets, never an opinion.
import { computed, reactive, ref, watch } from 'vue'
import { browse } from '../engine.js'
import { runCrossRef, famFor, keyOf, XREF, xrefByKey, POOL_LIMIT } from '../crossref.js'
import { si } from '../units.js'
import { store, togglePin, isPinned, pinColor } from '../store.js'
import { trackEvent } from '../telemetry.js'


const original = ref(null)   // the chosen browse row (declared early: fam depends on it)
const family = computed(() => xrefByKey(store.family))
// The config actually in force: the family, with any per-original variant applied.
// `fam.key` stays the SHARD family (browse/drawer/pin key on it); only the
// comparison model changes.
const fam = computed(() => famFor(store.family, original.value))


// ── step 1: find + pick the original part ────────────────────────────────────
const search = ref('')
const searching = ref(false)
const searchRows = ref(null) // null = untouched; [] = no hits
const searchTotal = ref(0)
let searchToken = 0

watch(() => fam.value.key, () => {
  search.value = ''
  searchRows.value = null
  original.value = null
  marked.value = []
  result.value = null
  errorMsg.value = ''
})

async function runSearch() {
  const q = search.value.trim()
  if (q.length < 2) { searchRows.value = null; return }
  const token = ++searchToken
  searching.value = true
  try {
    const r = await browse(fam.value.key, { filters: { mpn: q }, sort: { field: 'mpn', dir: 'asc' }, limit: 15 })
    if (token !== searchToken) return
    searchRows.value = r.rows
    searchTotal.value = r.total
  } catch (e) {
    if (token !== searchToken) return
    searchRows.value = []
    errorMsg.value = e.message
  } finally {
    if (token === searchToken) searching.value = false
  }
}
let debounce = null
watch(search, () => { clearTimeout(debounce); debounce = setTimeout(runSearch, 250) })

function pickOriginal(r) {
  original.value = r
  searchRows.value = null
  search.value = ''
  result.value = null
  loadMfrs()
}

// ── step 2: mark target manufacturer(s) ──────────────────────────────────────
const mfrs = ref([])        // [[name, count]...] from the manufacturer facet
const mfrFilter = ref('')
const marked = ref([])
const sameType = ref(true)

async function loadMfrs() {
  try {
    const r = await browse(fam.value.key, { withFacets: true, facetTop: 500, limit: 0 })
    mfrs.value = r.facets.manufacturer?.values ?? []
  } catch (e) {
    errorMsg.value = e.message
  }
}
const mfrShown = computed(() => {
  const q = mfrFilter.value.trim().toLowerCase()
  return q ? mfrs.value.filter(([m]) => m.toLowerCase().includes(q)) : mfrs.value
})
function markAllOthers() {
  marked.value = mfrs.value.map(([m]) => m).filter((m) => m !== original.value?.manufacturer)
}

// ── step 3: run the ranker ───────────────────────────────────────────────────
const result = ref(null)   // { ranked, rowByKey, poolTotal, poolScored, origVerified, missing }
const errorMsg = ref('')
const busy = ref(false)
const maxResults = ref(25)


const canRun = computed(() => original.value && marked.value.length > 0 && !busy.value)

async function run() {
  const f = fam.value
  busy.value = true
  errorMsg.value = ''
  result.value = null
  trackEvent('crossref_run', { target: f.key, manufacturers: marked.value.length })
  try {
    // The ranking itself lives in crossref.js so the headless auditor runs this
    // exact path; the view only owns the busy/error/telemetry shell around it.
    const r = await runCrossRef({
      family: store.family, original: original.value, manufacturers: marked.value,
      sameType: sameType.value, maxResults: maxResults.value,
    })
    result.value = r
    trackEvent('crossref_result', { target: f.key, pool: r.poolScored, ranked: r.ranked.length })
  } catch (e) {
    errorMsg.value = e.message
    trackEvent('crossref_error', { target: f.key, message: (e.message || '').slice(0, 120) })
  } finally {
    busy.value = false
  }
}

// ── render helpers ───────────────────────────────────────────────────────────
function rowOf(c) { return result.value?.rowByKey.get(c._key ?? c.mpn) }
function dispMpn(c) { return rowOf(c)?.mpn ?? c.mpn }
function dispMfr(c) { return rowOf(c)?.manufacturer ?? '' }
function verdictOf(c, paramKey) {
  return c.params?.find((p) => p.name === paramKey)?.verdict ?? 'unverified'
}
function cellValue(r, p) {
  const v = r?.[p.row]
  return si(v != null && p.scale ? v * p.scale : v, p.unit)
}
function penaltyText(c) {
  if (c.status === 'no_substitute') return '—'
  return c.penalty.toFixed(2)
}
const STATUS_LABEL = { recommended: 'recommended', partial: 'partial', no_substitute: 'no substitute' }
// Grade answers "what work does this swap cost me?" — the question the industry
// graders (SiliconExpert A/B/C/D/SF, Z2Data Drop-In A/B/C) exist to answer.
const GRADE_LABEL = {
  drop_in: 'drop-in', minor_review: 'minor review', major_review: 'major review',
  redesign: 'redesign', no_substitute: 'no substitute',
}
const GRADE_TITLE = {
  drop_in: 'fits the original’s footprint with no parameter regressions',
  minor_review: 'fits, but with warnings worth checking',
  major_review: 'fits, but a parameter regressed materially — re-qualify before building',
  redesign: 'does not fit the original’s board space, or mount/family/process differs',
  no_substitute: 'a hard gate failed — this is not a substitute',
}
// Direction answers "better or worse?", computed from the measured ratios.
const DIRECTION_LABEL = { upgrade: '▲ upgrade', downgrade: '▼ downgrade', mixed: '◆ mixed' }
const DIRECTION_TITLE = {
  upgrade: 'ahead of the original on every directional parameter compared',
  downgrade: 'behind the original on the parameters compared',
  mixed: 'better on some parameters, worse on others',
}
const FOOTPRINT_LABEL = {
  fits: 'fits', smaller: 'smaller', different_case: 'diff. case', one_size_larger: '+1 size',
  overflows: 'larger', different_land_pattern: 'diff. pads', unknown: '?',
}
// The LAND pattern only — which pads the part sits on. Height is a separate
// question (clearance above the board) and is reported in the notes, so neither
// axis can be read as an answer to the other.
const FOOTPRINT_TITLE = {
  fits: 'the land pattern fits within the original’s (orientation-agnostic); height is judged separately and reported in the notes',
  smaller: 'smaller land than the original — the pads differ, so it is not a drop-in; it needs a new land pattern',
  different_case: 'different package/case than the original — different land pattern (pinout/pads); a substitute to re-lay-out, not a drop-in',
  one_size_larger: 'about one case size larger on the board — works electrically, verify board space',
  overflows: 'larger land than the original’s footprint — board respin likely',
  different_land_pattern: 'the body outline matches but the pads do not: a multi-element resistor array and a discrete resistor do not share a land pattern',
  unknown: 'no dimensions or resolvable case code on this part',
}
function openPart(r) {
  if (typeof r?.srcOffset !== 'number') return
  store.drawer = { family: fam.value.key, mpn: r.mpn, manufacturer: r.manufacturer, srcOffset: r.srcOffset, srcLength: r.srcLength }
}
</script>

<template>
  <div class="xref">
    <div class="family-strip">
      <button
        v-for="f in XREF" :key="f.key"
        class="fam-tab" :class="{ active: f.key === fam.key }"
        type="button"
        @click="store.family = f.key"
      >{{ f.label }}</button>
    </div>

    <div class="split">
      <!-- setup rail -->
      <section class="setup panel">
        <p class="section-label">Original part — {{ fam.label }}</p>
        <p class="intro">Pick the part you need to replace, mark the manufacturer(s) you want the
        substitute from, and the deterministic cross-reference engine ranks their catalogue against
        the original — gates, penalties and honesty rules published, no AI.</p>
        <p v-if="fam.caveat" class="caveat">{{ fam.caveat }}</p>

        <label class="field">
          <span>Search by MPN</span>
          <input v-model="search" class="mpn-search" placeholder="e.g. 744373965" spellcheck="false" />
        </label>
        <div v-if="searchRows" class="hits">
          <p v-if="!searchRows.length" class="dimline">no MPN contains “{{ search.trim() }}”</p>
          <template v-else>
            <button
              v-for="r in searchRows" :key="`${r.manufacturer}|${r.mpn}`"
              class="hit" type="button" @click="pickOriginal(r)"
            >
              <span class="mono hit-mpn">{{ r.mpn }}</span>
              <span class="hit-mfr">{{ r.manufacturer }}</span>
              <span v-if="fam.primary" class="mono hit-val">{{ si(r[fam.primary.row], fam.primary.unit) }}</span>
            </button>
            <p v-if="searchTotal > searchRows.length" class="dimline">{{ searchTotal.toLocaleString() }} matches — keep typing to narrow</p>
          </template>
        </div>

        <div v-if="original" class="orig panel-inset" data-testid="original-card">
          <header class="orig-head">
            <button class="mpn-link mono" type="button" @click="openPart(original)">{{ original.mpn }}</button>
            <span class="mfr">{{ original.manufacturer }}</span>
            <button class="more" type="button" @click="original = null; result = null">change</button>
          </header>
          <dl class="orig-specs mono">
            <div v-for="p in fam.params" :key="p.key">
              <dt>{{ p.label }}</dt><dd>{{ cellValue(original, p) }}</dd>
            </div>
            <div v-if="fam.sameFacet && original[fam.sameFacet.f]">
              <dt>{{ fam.sameFacet.label }}</dt><dd>{{ original[fam.sameFacet.f] }}</dd>
            </div>
          </dl>
        </div>

        <template v-if="original">
          <p class="section-label">Substitute from</p>
          <input v-model="mfrFilter" placeholder="filter manufacturers…" spellcheck="false" />
          <div class="mfr-list" data-testid="mfr-list">
            <label v-for="[m, n] in mfrShown" :key="m" class="check mfr-row">
              <input v-model="marked" type="checkbox" :value="m" />
              <span class="mfr-name" :class="{ self: m === original.manufacturer }">
                {{ m }}<em v-if="m === original.manufacturer"> (original’s)</em>
              </span>
              <span class="mono mfr-n">{{ n.toLocaleString() }}</span>
            </label>
          </div>
          <div class="mfr-actions">
            <button class="more" type="button" @click="markAllOthers">mark all others</button>
            <button v-if="marked.length" class="more" type="button" @click="marked = []">clear</button>
          </div>

          <label v-if="fam.sameFacet && original[fam.sameFacet.f]" class="check">
            <input v-model="sameType" type="checkbox" />
            <span>Same {{ fam.sameFacet.label }} only ({{ original[fam.sameFacet.f] }})</span>
          </label>

          <label class="field">
            <span>Show top</span>
            <select v-model="maxResults">
              <option :value="10">10</option>
              <option :value="25">25</option>
              <option :value="50">50</option>
            </select>
          </label>

          <button class="primary run" type="button" :disabled="!canRun" @click="run()">
            {{ busy ? 'cross-referencing…' : 'Find cross references' }}
          </button>
          <p v-if="original && !marked.length" class="dimline">mark at least one manufacturer</p>
        </template>
      </section>

      <!-- ranked substitutes -->
      <section class="cands panel">
        <p class="section-label">
          Cross references
          <template v-if="result">
            — <span class="mono">{{ result.ranked.length }}</span> ranked
            · <span class="mono">{{ result.poolScored.toLocaleString() }}</span> scored
            <template v-if="result.poolTotal > result.poolScored + 1">
              of <span class="mono">{{ result.poolTotal.toLocaleString() }}</span> pre-gate matches (pool cap {{ POOL_LIMIT }})
            </template>
          </template>
        </p>

        <p v-if="errorMsg" class="err mono">{{ errorMsg }}</p>

        <div v-if="!original" class="idle">
          <p>Search the catalogue for the part you need to replace. Cross-referencing compares the
          original's datasheet numbers against every candidate from the manufacturers you mark:
          the primary value must sit inside the category's acceptance window, critical ratings
          (V<sub>ds</sub>, V<sub>rrm</sub>, rated voltage, I<sub>sat</sub>…) gate hard, and everything
          else prices in as a continuous penalty — lowest total penalty wins.</p>
        </div>
        <div v-else-if="!result && !errorMsg" class="idle">
          <p>Mark the manufacturer(s) you want the substitute from and run
          <b>Find cross references</b>.</p>
        </div>

        <template v-else-if="result">
          <p v-if="!result.origVerified" class="honesty mono" data-testid="honesty-banner">
            original's {{ result.missing.join(', ') }} not documented — candidates are capped at
            “partial” (a substitute can't be cleanly recommended against an unverified original)
          </p>

          <p v-if="!result.ranked.length" class="idle">
            No candidate from the marked manufacturer{{ marked.length > 1 ? 's' : '' }} passes the
            pre-gates<template v-if="fam.primary"> ({{ fam.primary.label }} within
            {{ fam.primary.acceptLo }}×–{{ fam.primary.acceptHi }}× of the original</template><template v-if="fam.hardMin?.length">, critical ratings ≥ 0.9× the original</template><template v-if="fam.exactNum?.length">, matching {{ fam.exactNum.map((e) => e.row).join(' and ') }}</template>).
            Widen the manufacturer marking or check the original's values.
          </p>

          <div v-else class="tbl-scroll">
            <table class="xref-tbl" data-testid="xref-table">
              <thead>
                <tr>
                  <th class="mono">#</th>
                  <th>MPN</th>
                  <th>Manufacturer</th>
                  <th>Status</th>
                  <th v-for="p in fam.params" :key="p.key" class="num">
                    {{ p.label }}<span class="orig-ref mono">{{ cellValue(original, p) }}</span>
                  </th>
                  <th title="does the substitute fit the original's board space?">fit</th>
                  <th class="num" title="total penalty — lower is better (0 = ideal)">penalty</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                <template v-for="(c, i) in result.ranked" :key="c._key ?? c.mpn">
                <tr :class="`row-${c.status}`">
                  <td class="mono rank">{{ String(i + 1).padStart(2, '0') }}</td>
                  <td><button class="mpn-link mono" type="button" @click="openPart(rowOf(c))">{{ dispMpn(c) }}</button></td>
                  <td class="mfr-cell">{{ dispMfr(c) }}</td>
                  <td>
                    <span class="chip" :class="`gr-${c.grade}`" :title="GRADE_TITLE[c.grade] ?? ''">{{ GRADE_LABEL[c.grade] ?? c.status }}</span>
                    <span v-if="c.direction && c.direction !== 'equivalent'" class="dir" :class="`dir-${c.direction}`"
                          :title="DIRECTION_TITLE[c.direction]">{{ DIRECTION_LABEL[c.direction] }}</span>
                  </td>
                  <td v-for="p in fam.params" :key="p.key" class="num mono" :class="`v-${verdictOf(c, p.key)}`"
                      :title="`${p.label}: ${verdictOf(c, p.key)}`">
                    {{ cellValue(rowOf(c), p) }}
                  </td>
                  <td :class="`fp-${c.footprint ?? 'unknown'}`" :title="FOOTPRINT_TITLE[c.footprint] ?? 'no dimensions on record'">
                    {{ FOOTPRINT_LABEL[c.footprint] ?? '—' }}
                  </td>
                  <td class="num mono">{{ penaltyText(c) }}</td>
                  <td>
                    <button
                      class="pin" type="button"
                      :style="isPinned(dispMpn(c)) ? { color: pinColor(dispMpn(c)), borderColor: pinColor(dispMpn(c)) } : {}"
                      @click="togglePin(fam.key, rowOf(c))"
                    >◉</button>
                  </td>
                </tr>
                <!-- the engine's own reasons for a demotion — footprint, automotive
                     grade, DC bias, lifecycle. Shown inline so a "partial" is never
                     unexplained. -->
                <tr v-if="c.notes?.length" class="notes-row">
                  <td></td>
                  <td :colspan="fam.params.length + 5" class="note">{{ c.notes.join(' · ') }}</td>
                </tr>
                </template>
              </tbody>
            </table>
          </div>

          <p v-if="result.ranked.length" class="legend">
            <span class="v-pass">■</span> pass · <span class="v-warn">■</span> warn ·
            <span class="v-fail">■</span> fail · <span class="v-unverified">■</span> not documented —
            per-parameter verdicts against the original ({{ fam.primary ? `${fam.primary.label} window + ` : '' }}directional gates,
            penalties in log-ratio space; “no substitute” = a hard gate failed).
            Also checked, and reported in the row note when they bite: physical fit
            (3-axis, orientation-agnostic, case codes resolved per family), SMD↔leaded
            mount type, automotive (AEC-Q) grade decoded from the MPN, and lifecycle.
          </p>
        </template>
      </section>
    </div>
  </div>
</template>

<style scoped>
.xref { display: flex; flex-direction: column; gap: 14px; flex: 1; min-height: 0; }
.family-strip { display: flex; flex-wrap: wrap; gap: 6px; }
.fam-tab {
  font-family: var(--disp);
  font-size: 10px;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  padding: 7px 12px;
}
.fam-tab.active { border-color: var(--k-deep); color: var(--k-hi); box-shadow: 0 0 10px rgba(127, 201, 255, 0.1) inset; }

.split { display: grid; grid-template-columns: 340px 1fr; gap: 14px; flex: 1; min-height: 0; }
.setup { padding: 16px; display: flex; flex-direction: column; gap: 12px; overflow-y: auto; max-height: calc(100vh - 210px); }
.intro { font-size: 11px; color: var(--ink-dim); margin: 0; }
.caveat { font-size: 11px; color: var(--warm); border-left: 2px solid var(--warm); padding-left: 8px; margin: 0; }
.field { display: flex; flex-direction: column; gap: 4px; font-size: 11px; }
.field span { color: var(--ink-dim); }
.check { display: flex; align-items: center; gap: 8px; font-size: 11px; color: var(--ink-dim); cursor: pointer; }
.check input { accent-color: var(--k); }
.dimline { font-size: 10px; color: #4a5b6e; margin: 2px 0 0; }
.run { margin-top: 4px; }
.more { font-size: 10px; padding: 2px 8px; }

.hits { display: flex; flex-direction: column; gap: 2px; }
.hit {
  display: flex; align-items: baseline; gap: 8px; text-align: left;
  padding: 5px 8px; font-size: 11px; background: var(--bg-deep);
}
.hit:hover { border-color: var(--k-deep); }
.hit-mpn { color: var(--k-hi); }
.hit-mfr { color: var(--ink-dim); flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.hit-val { color: var(--ink-dim); font-size: 10px; }

.panel-inset { border: 1px solid var(--line-soft); border-radius: 6px; padding: 10px 12px; background: var(--bg-deep); }
.orig-head { display: flex; align-items: center; gap: 10px; }
.orig-head .more { margin-left: auto; }
.mpn-link { background: none; border: none; padding: 0; color: var(--k-hi); font-size: 13px; cursor: pointer; }
.mpn-link:hover { text-decoration: underline; color: var(--k-hi); }
.mfr { color: var(--ink-dim); font-size: 11px; }
.orig-specs { display: flex; flex-wrap: wrap; gap: 4px 16px; margin: 8px 0 0; font-size: 11px; }
.orig-specs div { display: flex; gap: 6px; }
.orig-specs dt { color: var(--ink-dim); }
.orig-specs dd { margin: 0; color: var(--ink); }

.mfr-list {
  display: flex; flex-direction: column; gap: 2px; max-height: 230px; overflow-y: auto;
  border: 1px solid var(--line-soft); border-radius: 6px; padding: 6px; background: var(--bg-deep);
}
.mfr-row { padding: 1px 2px; }
.mfr-name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mfr-name.self { color: var(--warm); }
.mfr-name em { font-style: normal; font-size: 9px; color: #4a5b6e; }
.mfr-n { font-size: 10px; color: #4a5b6e; }
.mfr-actions { display: flex; gap: 8px; }

.cands { padding: 16px; overflow-y: auto; max-height: calc(100vh - 210px); }
.idle { color: var(--ink-dim); font-size: 12px; max-width: 520px; }
.err { color: var(--fault); font-size: 12px; }
.honesty { font-size: 11px; color: var(--warm); border: 1px solid rgba(255, 179, 71, 0.35); border-radius: 6px; padding: 6px 10px; margin: 0 0 10px; }

.tbl-scroll { overflow-x: auto; }
.xref-tbl { width: 100%; border-collapse: collapse; font-size: 12px; }
.xref-tbl th {
  text-align: left; font-family: var(--disp); font-size: 9px; letter-spacing: 0.08em;
  text-transform: uppercase; color: var(--ink-dim); padding: 6px 10px;
  border-bottom: 1px solid var(--line); white-space: nowrap;
}
.xref-tbl th.num, .xref-tbl td.num { text-align: right; }
.orig-ref { display: block; font-size: 9px; color: #4a5b6e; text-transform: none; letter-spacing: 0; }
.xref-tbl td { padding: 6px 10px; border-bottom: 1px solid var(--line-soft); white-space: nowrap; }
.rank { color: var(--k-deep); font-size: 11px; }
.mfr-cell { color: var(--ink-dim); font-size: 11px; }
.row-no_substitute { opacity: 0.55; }
.chip.st-recommended { color: var(--ok); border-color: rgba(120, 220, 150, 0.4); }
.chip.gr-drop_in { color: var(--ok); border-color: rgba(120, 220, 150, 0.45); }
.chip.gr-minor_review { color: var(--warm); border-color: rgba(255, 179, 71, 0.4); }
.chip.gr-major_review { color: var(--warm); border-color: rgba(255, 179, 71, 0.55); }
.chip.gr-redesign { color: var(--fault); border-color: rgba(255, 110, 110, 0.4); }
.chip.gr-no_substitute { color: var(--fault); border-color: rgba(255, 110, 110, 0.5); }
.dir { font-size: 9px; margin-left: 6px; white-space: nowrap; }
.dir-upgrade { color: var(--ok); }
.dir-downgrade { color: var(--fault); }
.dir-mixed { color: var(--ink-dim); }
.chip.st-partial { color: var(--warm); border-color: rgba(255, 179, 71, 0.4); }
.chip.st-no_substitute { color: var(--fault); border-color: rgba(255, 110, 110, 0.4); }
.v-pass { color: var(--ok); }
.v-warn { color: var(--warm); }
.v-fail { color: var(--fault); }
.v-unverified { color: #4a5b6e; }
.xref-tbl td[class^="fp-"] { font-size: 10px; }
.fp-fits { color: var(--ok); }
.fp-smaller { color: var(--warm); }
.fp-different_case { color: var(--warm); }
.fp-one_size_larger { color: var(--warm); }
.fp-overflows { color: var(--fault); }
.fp-different_land_pattern { color: var(--fault); }
.fp-unknown { color: #4a5b6e; }
.notes-row td { padding-top: 0; border-bottom: 1px solid var(--line-soft); }
.note { font-size: 10px; color: var(--warm); }
.pin { padding: 1px 7px; border-radius: 999px; font-size: 12px; }
.legend { font-size: 10px; color: var(--ink-dim); margin: 10px 0 0; }

@media (max-width: 1100px) {
  .split { grid-template-columns: 1fr; }
  .setup, .cands { max-height: none; }
}
</style>
