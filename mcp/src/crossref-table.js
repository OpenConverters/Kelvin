/**
 * The cross-reference table — the MCP App behind Faraday's crossref_board (and, as an
 * identical copy in Kelvin's repo, Kelvin's crossref_bom).
 *
 * WHY THIS EXISTS. Without it the only way a reader saw a board's cross-reference was the
 * model re-typing it: on the PoE reference board (189 positions) that was a 119-row
 * markdown table and 17,646 output tokens — two minutes of a chat writing out what the tool
 * had already computed in seconds. The table is the tool's result, so the tool's result
 * draws it, and the model is left to say what in it matters.
 *
 * What it shows is the `bom` payload exactly as crossref_board sends it, one row per GROUP
 * of positions that got the same answer (24 identical 100 nF capacitors are one row with
 * 24 designators), sectioned by the contract's line status, sortable, filterable, and
 * searchable. Nothing is computed here that the payload does not say: no grade is
 * inferred, no status re-derived. A status or grade this file does not know is drawn as
 * unknown (dashed), not mapped to the nearest one it does.
 *
 * Clicking a row reports that group to the model, so the next question can be about it.
 */
import { App } from "@modelcontextprotocol/ext-apps";

const app = new App({ name: "Cross-reference table", version: "0.1.0" });

// The contract's statuses, in the order a reader triages them.
const STATUS_ORDER = ["recommended", "partial", "no_substitute", "unsourced", "exact"];
const STATUS_WORDS = {
  exact: "Same part", recommended: "Recommended", partial: "Partial",
  no_substitute: "No substitute", unsourced: "Not cross-referenced",
};
const GRADE_WORDS = {
  drop_in: "drop-in", minor_review: "minor review", major_review: "major review",
  redesign: "redesign",
};
// How a line was identified (`_match`), as crossref_board's caveat defines it.
const MATCH_WORDS = {
  exact: "part number", substring: "partial part number", "value-package": "value + package",
  none: "not in catalogue", unlookupable: "no part number or value",
  "not-a-part": "not a part",
};

const state = {
  payload: null, groups: [], error: "",
  filter: "all", query: "", sort: { key: "status", dir: 1 }, chosen: "", crossref: "",
  loading: "",
};

// ── shaping ────────────────────────────────────────────────────────────────

/**
 * One payload line as the fields the table shows. Every field is read, not inferred.
 *
 * TWO PRODUCERS, ONE CONTRACT MODE. Faraday's crossref_board and Kelvin's crossref_bom both
 * answer in the contract's `bom` mode, and this file is shipped by both servers (each from
 * its own repo). They agree on the contract fields (ref, status, mpn, manufacturer,
 * originalMpn, kind, value, specs, notes) and differ in their underscore extras:
 *
 *   Faraday crossref_board          Kelvin crossref_bom
 *   _grade, _flags                  candidates[0].grade, candidates[0].params[].verdict
 *   _match                          _identification.certainty
 *   _originalManufacturer           (not carried)
 *   _alternates                     candidates.length - 1 + _candidatesHeldBack
 *   every line answers for itself   _sameAs: <ref> — the answer is on that earlier line
 *
 * A `_sameAs` naming a line that is not in the payload throws, and the widget says so: a
 * row drawn without its answer would read as an answer.
 */
function shape(line, target, byRef) {
  let base = line;
  if (line._sameAs) {
    base = byRef.get(line._sameAs);
    if (!base) throw new Error(`${line.ref} points at ${line._sameAs}, which is not in the payload`);
  }
  const specs = line.specs ?? {};
  const cands = base.candidates ?? [];
  const best = cands.length && cands[0].status !== undefined ? cands[0] : null;
  const flags = line._flags
    ?? (best?.params ?? []).filter((p) => p.verdict === "warn" || p.verdict === "fail").map((p) => p.name);
  return {
    ref: line.ref,
    // A merged line (Kelvin: the same part on several rows of a designator-less BOM) carries
    // every reference in _refs and the summed quantity in _quantity.
    refs: line._refs ?? [line.ref],
    quantity: line._quantity ?? null,
    status: line.status,
    original: line.originalMpn ?? null,
    originalMaker: line._originalManufacturer ?? null,
    kind: line.kind ?? null,
    value: line.value ?? null,
    pkg: specs.package ?? specs.footprint ?? null,
    sub: line.mpn ?? null,
    subMaker: line.mpn ? (line.manufacturer ?? target ?? null) : null,
    grade: line._grade ?? best?.grade ?? null,
    flags,
    match: line._match ?? line._identification?.certainty ?? null,
    // Kelvin opens every note with the identification it already carries in _identification;
    // the table has a column for that, so the sentence after it is what is shown.
    notes: (base.notes ?? "").replace(/^identification: [^—]*— /, ""),
    alternates: line._alternates
      ?? (best ? cands.length - 1 + (base._candidatesHeldBack ?? 0) : 0),
    rows: line._rows ?? (cands.length && !best ? cands.length + (base._rowsHeldBack ?? 0) : 0),
  };
}

const naturalRef = (a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" });

/** "C1, C2, C3, C5" -> "C1–C3, C5": consecutive numbers under one prefix collapse. */
function compactRefs(refs) {
  const sorted = [...refs].sort(naturalRef);
  const out = [];
  let run = null;
  const flush = () => {
    if (!run) return;
    out.push(run.to - run.from >= 2 ? `${run.prefix}${run.from}–${run.prefix}${run.to}`
      : run.to > run.from ? `${run.prefix}${run.from}, ${run.prefix}${run.to}`
      : `${run.prefix}${run.from}`);
    run = null;
  };
  for (const r of sorted) {
    const m = /^(.*?)(\d+)$/.exec(r);
    if (!m) { flush(); out.push(r); continue; }
    const [prefix, n] = [m[1], Number(m[2])];
    if (run && run.prefix === prefix && n === run.to + 1 && String(n) === m[2]) run.to = n;
    else { flush(); run = { prefix, from: n, to: n }; }
    if (String(n) !== m[2]) flush();          // a zero-padded number never joins a range
  }
  flush();
  return out.join(", ");
}

/** Positions that got the same answer, as one row. */
function group(lines) {
  const byKey = new Map();
  for (const l of lines) {
    const { ref, refs, quantity, ...answer } = l;
    const key = JSON.stringify(answer);
    if (!byKey.has(key)) byKey.set(key, { ...answer, refs: [], quantity: null, counted: true });
    const g = byKey.get(key);
    g.refs.push(...refs);
    // A quantity is shown only when every line in the group states one; otherwise the
    // positions are counted (a designator BOM) or the cell says nothing (a line-id BOM).
    if (quantity === null) g.counted = false; else g.quantity = (g.quantity ?? 0) + quantity;
  }
  return [...byKey.values()].map((g) => ({
    ...g, refs: g.refs.sort(naturalRef), refText: compactRefs(g.refs), id: g.refs[0],
    qty: g.counted ? g.quantity : g.refs.length,
  }));
}

const statusRank = (s) => { const i = STATUS_ORDER.indexOf(s); return i < 0 ? 99 : i; };
const GRADE_RANK = { drop_in: 0, minor_review: 1, major_review: 2, redesign: 3 };

const SORTS = {
  status: (a, b) => statusRank(a.status) - statusRank(b.status)
    || (GRADE_RANK[a.grade] ?? 9) - (GRADE_RANK[b.grade] ?? 9) || naturalRef(a.id, b.id),
  refs: (a, b) => naturalRef(a.id, b.id),
  qty: (a, b) => (a.qty ?? 0) - (b.qty ?? 0),
  original: (a, b) => String(a.original ?? a.value ?? "").localeCompare(String(b.original ?? b.value ?? "")),
  sub: (a, b) => String(a.sub ?? "").localeCompare(String(b.sub ?? "")),
  grade: (a, b) => (GRADE_RANK[a.grade] ?? 9) - (GRADE_RANK[b.grade] ?? 9),
  pkg: (a, b) => String(a.pkg ?? "").localeCompare(String(b.pkg ?? ""), undefined, { numeric: true }),
};

// ── rendering ──────────────────────────────────────────────────────────────

function el(tag, attrs = {}, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === null || v === undefined || v === false) continue;
    if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
    else if (k === "class") n.className = v;
    else n.setAttribute(k, v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined || kid === false) continue;
    n.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return n;
}

function positions(status) {
  return state.groups.filter((g) => status === "all" || g.status === status)
    .reduce((n, g) => n + g.refs.length, 0);
}

function visible() {
  const q = state.query.trim().toLowerCase();
  return state.groups.filter((g) => (state.filter === "all" || g.status === state.filter)
    && (!q || [g.refs.join(" "), g.original, g.originalMaker, g.sub, g.subMaker, g.value,
               g.pkg, g.kind, g.notes].some((s) => s && String(s).toLowerCase().includes(q))));
}

function headerCell(key, label) {
  const on = state.sort.key === key;
  return el("th", {
    onclick: () => {
      state.sort = { key, dir: on ? -state.sort.dir : 1 };
      render();
    },
  }, label, on ? el("span", { class: "dir" }, state.sort.dir > 0 ? "▲" : "▼") : null);
}

function row(g) {
  const reason = g.sub ? null
    : g.notes || (g.match ? `identified by: ${MATCH_WORDS[g.match] ?? g.match}` : null);
  return el("tr", {
    class: `row ${g.status}${g.id === state.chosen ? " chosen" : ""}`,
    "data-id": g.id,
    onclick: () => choose(g),
  },
  el("td", { class: "mono refs" }, g.refText),
  el("td", { class: "qty" }, g.qty ?? "—"),
  el("td", {},
    el("div", { class: "mono" }, g.original ?? (g.value ? `value ${g.value}` : "—")),
    g.originalMaker || g.kind
      ? el("div", { class: "maker" }, [g.originalMaker, g.kind].filter(Boolean).join(" · ")) : null),
  el("td", {},
    g.sub ? el("div", { class: "mono" }, g.sub) : el("span", { class: `badge ${g.status}` },
      STATUS_WORDS[g.status] ?? g.status),
    g.sub && g.subMaker ? el("div", { class: "maker" }, g.subMaker
      + (g.alternates ? ` · +${g.alternates} alternative${g.alternates === 1 ? "" : "s"}` : "")) : null,
    reason ? el("div", { class: "note" }, reason) : null),
  el("td", {},
    g.sub ? el("span", { class: `badge ${g.status}` }, STATUS_WORDS[g.status] ?? g.status) : null,
    g.grade ? el("div", { class: `grade ${g.grade}` }, GRADE_WORDS[g.grade] ?? g.grade) : null),
  el("td", {}, g.flags.map((f) => el("span", { class: "flag", title: "warned or failed on" }, f))),
  el("td", { class: "mono" }, g.pkg ?? "—"),
  el("td", { class: "maker" }, g.match ? (MATCH_WORDS[g.match] ?? g.match) : "—"));
}

function render() {
  const root = document.getElementById("app");
  if (state.error) { root.replaceChildren(el("div", { class: "err" }, state.error)); return; }
  const p = state.payload;
  if (!p || state.loading) {
    root.replaceChildren(el("div", { class: "muted" }, state.loading || "Waiting for the cross-reference…"));
    return;
  }

  const statuses = STATUS_ORDER.filter((s) => state.groups.some((g) => g.status === s));
  const unknown = [...new Set(state.groups.map((g) => g.status))].filter((s) => !STATUS_ORDER.includes(s));
  const chip = (s, label) => el("button", {
    type: "button", class: `chip ${s}${state.filter === s ? " on" : ""}`,
    onclick: () => { state.filter = s; render(); },
  }, el("span", { class: "n" }, positions(s)), label);

  const shown = visible();
  const sorter = SORTS[state.sort.key];
  shown.sort((a, b) => state.sort.dir * sorter(a, b));
  // Sectioned by status only in the default order; any other sort is one flat list, since
  // section breaks across a list sorted by package would mean nothing.
  const sectioned = state.sort.key === "status";
  const body = [];
  let last = null;
  for (const g of shown) {
    if (sectioned && g.status !== last) {
      const n = shown.filter((x) => x.status === g.status).reduce((k, x) => k + x.refs.length, 0);
      body.push(el("tr", { class: "group" }, el("td", { colspan: 8 },
        `${STATUS_WORDS[g.status] ?? g.status} — ${n} position${n === 1 ? "" : "s"}`)));
      last = g.status;
    }
    body.push(row(g));
  }

  const target = p.targetManufacturer;
  const count = (pred) => state.groups.filter(pred).reduce((n, g) => n + g.refs.length, 0);
  const withSub = count((g) => g.status === "recommended" || g.status === "partial");
  const already = count((g) => g.status === "exact");
  root.replaceChildren(
    el("div", { class: "head" },
      el("h1", {}, `${p.total} position${p.total === 1 ? "" : "s"} · ${withSub} with a`
        + (target ? ` ${target}` : "") + " substitute"
        + (already ? ` · ${already} already ${target ?? "the same part"}` : "")),
      el("div", { class: "sub" },
        `${state.groups.length} distinct answers. Kelvin's deterministic ranker; click a row `
        + "and your choice goes back to the assistant."),
      el("div", { class: "bar" },
        chip("all", "all"),
        statuses.map((s) => chip(s, STATUS_WORDS[s])),
        unknown.map((s) => chip(s, s)),
        el("input", {
          class: "q", type: "search", placeholder: "Filter: designator, part, maker…",
          value: state.query,
          oninput: (e) => {
            state.query = e.target.value;
            const pos = e.target.selectionStart;
            render();
            const q = document.querySelector("input.q");
            q.focus();
            q.setSelectionRange(pos, pos);
          },
        }))),
    el("div", { class: "scroll" }, el("table", {},
      el("thead", {}, el("tr", {},
        headerCell("refs", "Designators"), headerCell("qty", "Qty"),
        headerCell("original", "Original part"),
        headerCell("sub", target ? `${target} substitute` : "Substitute"),
        headerCell("status", "Status / grade"), el("th", {}, "Flags"),
        headerCell("pkg", "Package"), el("th", {}, "Identified by"))),
      el("tbody", {}, body))),
    (p.diagnostics ?? []).length
      ? el("div", { class: "foot" }, `${p.diagnostics.length} diagnostic(s): `
          + p.diagnostics.slice(0, 3).join(" · ") + (p.diagnostics.length > 3 ? " …" : ""))
      : null);
}

// ── host wiring ────────────────────────────────────────────────────────────

async function choose(g) {
  state.chosen = g.id;
  render();
  const text = [
    `[user selected] ${g.refText} (${g.refs.length} position${g.refs.length === 1 ? "" : "s"})`
      + ` — status ${g.status}.`,
    `[original] ${g.original ?? "(no part number)"}${g.originalMaker ? ` (${g.originalMaker})` : ""}`
      + (g.value ? `, value ${g.value}` : "") + (g.pkg ? `, ${g.pkg}` : ""),
    g.sub ? `[substitute] ${g.sub}${g.subMaker ? ` (${g.subMaker})` : ""}`
      + (g.grade ? `, grade ${g.grade}` : "") + (g.flags.length ? `, flags ${g.flags.join(", ")}` : "")
      : null,
    g.notes ? `[notes] ${g.notes}` : null,
    state.crossref ? `[full line] ${state.lineTool}(crossref='${state.crossref}', ref='${g.id}')` : null,
  ].filter(Boolean).join("\n");
  await app.updateModelContext({
    content: [{ type: "text", text }],
    structuredContent: JSON.parse(JSON.stringify({ selected: g })),
  });
}

/**
 * Every line of the cross-reference. A BOM too long to carry inline (Kelvin's crossref_bom
 * above ~45k characters) arrives with its totals and no lines; they are then fetched from the
 * server a page at a time with crossref_bom_lines, so the table still shows all of them.
 */
async function allLines(sc) {
  if (sc.lines.length >= sc.total || !state.crossref) return sc.lines;
  const lines = [];
  const seen = new Set();
  for (let offset = 0; offset < sc.total; offset += 1000) {
    state.loading = `Loading lines ${offset + 1}–${Math.min(offset + 1000, sc.total)} of ${sc.total}…`;
    render();
    const r = await app.callServerTool({
      name: "crossref_bom_lines", arguments: { crossref: state.crossref, offset, limit: 1000 },
    });
    if (r.isError) throw new Error(`crossref_bom_lines failed: ${(r.content ?? []).map((c) => c.text).join(" ")}`);
    const page = r.structuredContent?.lines;
    if (!Array.isArray(page) || !page.length) throw new Error(`crossref_bom_lines returned no lines at offset ${offset}`);
    for (const l of page) if (!seen.has(l.ref)) { seen.add(l.ref); lines.push(l); }
  }
  state.loading = "";
  if (lines.length !== sc.total) throw new Error(`loaded ${lines.length} lines, the BOM has ${sc.total}`);
  return lines;
}

app.ontoolresult = async (result) => {
  const sc = result?.structuredContent;
  if (sc?.mode !== "bom" || !Array.isArray(sc.lines)) {
    state.error = "The tool returned no cross-reference for this widget.";
    render();
    return;
  }
  state.payload = sc;
  // The run's id is stated in the caveat (crossref_board has no other field for it); a
  // payload without one simply gets no crossref_line pointer.
  const handle = /(crossref(?:_bom)?_line)s?\(crossref='([0-9a-f]+)'/.exec(sc.caveat ?? "");
  state.crossref = handle?.[2] ?? "";
  state.lineTool = handle?.[1] ?? "crossref_line";
  try {
    const lines = await allLines(sc);
    const byRef = new Map(lines.map((l) => [l.ref, l]));
    state.groups = group(lines.map((l) => shape(l, sc.targetManufacturer, byRef)));
    state.error = "";
    render();
  } catch (err) {
    // The SDK swallows a handler's exception, so without this a payload the table cannot
    // draw would leave "Waiting…" on screen forever. Say so, on screen and in the console.
    console.error("crossref table:", err);
    state.error = `Could not draw this cross-reference: ${err.message}`;
    render();
  }
};

/** Wear the host's theme (see board.js's wearHostTheme for why not prefers-color-scheme). */
function wearHostTheme(theme) {
  document.documentElement.dataset.theme = theme === "light" ? "light" : "dark";
}

render();
app.onhostcontextchanged = (ctx) => wearHostTheme(ctx?.theme);
await app.connect();
wearHostTheme(app.getHostContext()?.theme);
