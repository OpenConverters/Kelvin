<script setup>
// Home — what Kelvin is, what it holds, and the ways in. Every catalogue figure comes
// from src/generated/catalog-summary.json, written at build time by
// scripts/build-home-summary.mjs running the shipped WASM engine over the served
// shards (the same browse() queries StatsView runs live). This view downloads no
// shard and no NDJSON; it only reads the 3 KB manifest to confirm the summary
// describes the data this host is serving.
import { computed, onMounted, ref } from 'vue'
import summary from '../generated/catalog-summary.json'
import { manifest } from '../engine.js'
import { FAMILIES } from '../families.js'
import { XREF } from '../crossref.js'
import { pct } from '../units.js'

const XREF_KEYS = new Set(XREF.map((x) => x.key))

const cards = computed(() => summary.families.map((s) => {
  const f = FAMILIES.find((x) => x.key === s.key)
  if (!f) throw new Error(`catalog summary lists family '${s.key}' that families.js does not define`)
  const topMax = Math.max(...s.topManufacturers.map((m) => m.count), 1)
  return {
    ...s,
    label: f.label,
    tagline: f.tagline,
    glyph: f.glyph,
    canRecommend: !!f.recommend,
    canCrossRef: XREF_KEYS.has(s.key),
    prodShare: s.production == null ? null : s.production / s.parts,
    top: s.topManufacturers.map((m) => ({ ...m, share: m.count / s.parts, w: m.count / topMax })),
  }
}))

// largest families first: the grid reads as "where the parts are"
const byParts = computed(() => [...cards.value].sort((a, b) => b.parts - a.parts))

// Is the summary the one for the data this host serves? (buildIds are content hashes)
const drift = ref(null) // null = unchecked / consistent; [] of family keys that differ
const manifestError = ref('')
onMounted(async () => {
  try {
    const m = await manifest()
    const differ = Object.keys({ ...m.families, ...summary.buildIds })
      .filter((k) => String(m.families[k]?.buildId) !== String(summary.buildIds[k]))
    drift.value = differ.length ? differ : null
  } catch (e) {
    manifestError.value = e.message
  }
})

const fmt = (n) => n.toLocaleString('en-US')
</script>

<template>
  <div class="home">
    <!-- ── purpose ─────────────────────────────────────────────────────────── -->
    <section class="hero panel">
      <div class="hero-text">
        <p class="section-label">the shared parts librarian · OpenConverters</p>
        <h2>Real parts, ranked the same way every time.</h2>
        <p class="lede">
          Kelvin is a deterministic component selector over the TAS database of real
          catalogue parts. Give it a <code>designRequirements</code> block and it returns a
          ranked, auditable list of candidates — with the margin each part holds against every
          requirement and a count of why the rest were turned away. No AI in the loop: the same
          question always gets the same answer.
        </p>
        <div class="cta">
          <a class="btn primary" href="#/catalog/magnetic">Browse the catalogue ›</a>
          <a class="btn" href="#/recommend/mosfet">Recommend a part ›</a>
          <a class="btn" href="#/crossref/mosfet">Cross-reference ›</a>
          <a class="btn" href="#/stats/capacitor">Stats ›</a>
        </div>
      </div>
      <dl class="headline mono" aria-label="catalogue totals">
        <div><dt>parts indexed</dt><dd>{{ fmt(summary.totals.parts) }}</dd></div>
        <div><dt>families</dt><dd>{{ summary.totals.families }}</dd></div>
        <div><dt>manufacturer names</dt><dd>{{ summary.totals.manufacturerNames }}</dd></div>
      </dl>
    </section>

    <!-- ── principles ──────────────────────────────────────────────────────── -->
    <section class="principles" aria-label="what Kelvin guarantees">
      <article class="panel principle">
        <p class="section-label">deterministic</p>
        <h3>Same question, same answer</h3>
        <p>
          Ordering is stable down to the source line number. The same catalogue file builds a
          byte-identical index shard, and the browser engine returns exactly what the native
          library does.
        </p>
      </article>
      <article class="panel principle">
        <p class="section-label">auditable</p>
        <h3>Nothing disappears silently</h3>
        <p>
          Every part that does not qualify is counted in a rejection histogram by reason — even
          a row the index could not read is counted, not hidden. Candidates carry their margins
          (rated ÷ required) so the ranking can be checked, not trusted.
        </p>
      </article>
      <article class="panel principle">
        <p class="section-label">ranked, with verdicts</p>
        <h3>Imperfect matches still show up</h3>
        <p>
          Magnetics are ranked, never gated: the whole catalogue is ordered toward your targets
          and each dimension gets a
          <span class="verdict-pass">pass</span> / <span class="verdict-marginal">marginal</span> /
          <span class="verdict-fail">fail</span> verdict. Cross-reference gives every parameter of
          a substitute its own verdict against the original part.
        </p>
      </article>
      <article class="panel principle">
        <p class="section-label">parity-locked</p>
        <h3>Ported, then proven equal</h3>
        <p>
          The MOSFET, diode, capacitor, resistor and controller selectors reproduce Heaviside's
          Python selector exactly — a differential fuzzer over the full database found 0
          mismatches. IGBT, BJT and varistor selectors are new and review-gated.
        </p>
      </article>
      <article class="panel principle">
        <p class="section-label">in your browser</p>
        <h3>No backend</h3>
        <p>
          A compact binary index per family loads into the WASM engine; selection and browsing
          run locally. A part's full datasheet record crosses the wire only when you open it, as
          a byte-range slice of a few kilobytes.
        </p>
      </article>
    </section>

    <!-- ── catalogue summary ───────────────────────────────────────────────── -->
    <section class="catalogue" aria-label="catalogue summary">
      <div class="cat-head">
        <p class="section-label">what the librarian holds</p>
        <p class="cat-note">
          Figures computed at build time by the Kelvin engine over the index shards this site
          serves. “Production” = parts whose status is recorded as <code>production</code>;
          parts with any other or no recorded status count against it.
        </p>
      </div>
      <p v-if="drift" class="drift mono">
        This summary was built from different index shards than this host serves
        ({{ drift.join(', ') }}) — rebuild the site (<code>npm run build</code>) to refresh the figures.
      </p>
      <p v-if="manifestError" class="drift mono">Could not check the summary against the served manifest: {{ manifestError }}</p>

      <div class="fam-grid">
        <article v-for="c in byParts" :key="c.key" class="fam panel">
          <header class="fam-head">
            <span class="glyph" aria-hidden="true">{{ c.glyph }}</span>
            <div class="fam-title">
              <h3>{{ c.label }}</h3>
              <p>{{ c.tagline }}</p>
            </div>
            <span class="chip" :class="{ on: c.canRecommend }">{{ c.canRecommend ? 'selector' : 'browse only' }}</span>
          </header>

          <div class="fam-figs">
            <div><span class="fig mono">{{ fmt(c.parts) }}</span><span class="fig-l">parts</span></div>
            <div><span class="fig mono">{{ c.manufacturers }}</span><span class="fig-l">manufacturers</span></div>
            <div v-if="c.prodShare != null">
              <span class="fig mono">{{ pct(c.prodShare, 1) }}</span><span class="fig-l">production</span>
            </div>
            <div v-else><span class="fig mono dim">—</span><span class="fig-l">status not indexed</span></div>
          </div>
          <span v-if="c.prodShare != null" class="prod-track" :title="`${fmt(c.production)} of ${fmt(c.parts)} marked production`">
            <i :style="{ width: pct(c.prodShare, 2) }" />
          </span>

          <p class="mini-label">top manufacturers</p>
          <div v-for="m in c.top" :key="m.name" class="mfr-row">
            <span class="mfr-name" :title="m.name">{{ m.name }}</span>
            <span class="mfr-track"><i :style="{ width: pct(m.w, 2) }" /></span>
            <span class="mfr-n mono">{{ pct(m.share) }}</span>
          </div>

          <template v-if="c.mix">
            <p class="mini-label">{{ c.mix.label.toLowerCase() }}<template v-if="c.mix.distinct > c.mix.top.length"> · top {{ c.mix.top.length }} of {{ c.mix.distinct }}</template></p>
            <div class="mix">
              <span v-for="t in c.mix.top" :key="t.value" class="chip">
                {{ t.value || 'not stated' }} <b class="mono">{{ fmt(t.count) }}</b>
              </span>
            </div>
          </template>

          <p v-if="c.unreadableRows" class="unreadable">
            + {{ fmt(c.unreadableRows) }} of {{ fmt(c.sourceLines) }} source rows unreadable by the index — counted, not indexed
          </p>

          <nav class="fam-links" :aria-label="`${c.label} views`">
            <a :href="`#/catalog/${c.key}`">Browse ›</a>
            <a v-if="c.canRecommend" :href="`#/recommend/${c.key}`">Recommend ›</a>
            <a v-if="c.canCrossRef" :href="`#/crossref/${c.key}`">Cross-ref ›</a>
            <a :href="`#/stats/${c.key}`">Stats ›</a>
          </nav>
        </article>
      </div>
    </section>
  </div>
</template>

<style scoped>
.home { display: flex; flex-direction: column; gap: 14px; }

/* shared link-as-button (global .btn only colours; give it block layout here) */
.btn { display: inline-flex; align-items: center; text-decoration: none; }
.btn:hover { text-decoration: none; }
.btn.primary { color: #05121e; background: var(--k); border-color: var(--k); font-weight: 500; }
.btn.primary:hover { background: var(--k-hi); color: #05121e; }

/* ── hero ─────────────────────────────────────────────────────────────── */
.hero {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 28px;
  padding: 28px 30px;
  align-items: center;
  overflow: hidden;
}
.hero h2 {
  margin: 0 0 14px;
  font-family: var(--disp);
  font-weight: 400;
  font-size: clamp(18px, 2.6vw, 28px);
  line-height: 1.35;
  letter-spacing: 0.04em;
  color: var(--k-hi);
  text-shadow: 0 0 14px rgba(127, 201, 255, 0.25);
}
.lede { margin: 0; max-width: 72ch; line-height: 1.6; color: var(--ink); }
.lede code { color: var(--k); font-size: 13px; }
.cta { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 20px; }
.headline {
  display: flex;
  flex-direction: column;
  gap: 14px;
  margin: 0;
  padding: 16px 22px;
  border: 1px solid var(--line);
  border-radius: 6px;
  background: var(--bg-deep);
}
.headline div { display: flex; flex-direction: column; gap: 2px; }
.headline dt { font-size: 9px; letter-spacing: 0.2em; text-transform: uppercase; color: var(--ink-dim); }
.headline dd { margin: 0; font-size: 26px; color: var(--k-hi); }


/* ── principles ───────────────────────────────────────────────────────── */
.principles { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 250px), 1fr)); gap: 14px; }
.principle { padding: 16px 18px; }
.principle .section-label { margin-bottom: 6px; color: var(--k); }
.principle h3 { margin: 0 0 8px; font-size: 15px; font-weight: 600; color: var(--ink); }
.principle p:last-child { margin: 0; font-size: 13px; line-height: 1.55; color: var(--ink-dim); }

/* ── catalogue ────────────────────────────────────────────────────────── */
.cat-head { display: flex; flex-wrap: wrap; align-items: baseline; justify-content: space-between; gap: 4px 20px; margin-top: 6px; }
.cat-head .section-label { margin: 0; }
.cat-note { margin: 0; font-size: 11px; color: var(--ink-dim); max-width: 80ch; }
.cat-note code { font-size: 11px; }
.drift { color: var(--warm); font-size: 12px; margin: 8px 0 0; }
.fam-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 330px), 1fr)); gap: 14px; margin-top: 12px; }
.fam { padding: 14px 16px; display: flex; flex-direction: column; gap: 6px; }
.fam-head { display: flex; align-items: center; gap: 12px; }
.glyph {
  flex: none;
  width: 34px; height: 34px;
  display: grid; place-items: center;
  font-family: var(--disp);
  font-size: 15px;
  color: var(--k);
  border: 1px solid var(--k-deep);
  border-radius: 5px;
  background: var(--bg-deep);
}
.fam-title { flex: 1; min-width: 0; }
.fam-title h3 { margin: 0; font-family: var(--disp); font-weight: 400; font-size: 12px; letter-spacing: 0.14em; text-transform: uppercase; color: var(--k-hi); }
.fam-title p { margin: 2px 0 0; font-size: 11px; color: var(--ink-dim); }
.fam-figs { display: flex; flex-wrap: wrap; gap: 6px 22px; margin-top: 6px; }
.fam-figs div { display: flex; flex-direction: column; }
.fig { font-size: 20px; color: var(--ink); }
.fig.dim { color: #4a5b6e; }
.fig-l { font-size: 9px; letter-spacing: 0.16em; text-transform: uppercase; color: var(--ink-dim); }
.prod-track { display: block; height: 6px; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 3px; overflow: hidden; }
.prod-track i { display: block; height: 100%; background: var(--ok); opacity: 0.7; }
.mini-label { margin: 8px 0 2px; font-size: 9px; letter-spacing: 0.16em; text-transform: uppercase; color: var(--ink-dim); }
.mfr-row { display: grid; grid-template-columns: minmax(0, 150px) 1fr 42px; gap: 8px; align-items: center; font-size: 11px; }
.mfr-name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mfr-track { height: 9px; background: var(--bg-deep); border: 1px solid var(--line-soft); border-radius: 2px; overflow: hidden; }
.mfr-track i { display: block; height: 100%; background: var(--s1); }
.mfr-n { text-align: right; font-size: 10px; color: var(--ink-dim); }
.mix { display: flex; flex-wrap: wrap; gap: 5px; }
.mix b { font-weight: 400; color: var(--ink); }
.unreadable { margin: 6px 0 0; font-size: 10px; color: var(--ink-dim); }
.fam-links { display: flex; flex-wrap: wrap; gap: 4px 14px; margin-top: auto; padding-top: 10px; border-top: 1px solid var(--line-soft); font-size: 12px; font-family: var(--mono); }

@media (max-width: 900px) {
  .hero { grid-template-columns: minmax(0, 1fr); padding: 20px 18px; }
  .headline { flex-direction: row; flex-wrap: wrap; gap: 10px 24px; }
  .headline dd { font-size: 20px; }
}
</style>
