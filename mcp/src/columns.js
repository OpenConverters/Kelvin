/**
 * How a row of candidates becomes a row of columns.
 *
 * Extracted from picker.js so it can be tested without a host bridge: the widget module
 * connects to the MCP Apps host at import, so anything left inside it can only be checked by
 * rendering the real GUI. The bug this file exists to prevent was exactly that — the ranked
 * views silently derived one column, and nobody noticed until someone rendered them.
 */

/**
 * Fields that are candidate *metadata*, not specs to put in the table.
 *
 * A tool that hands us pre-shaped `specs` decides its own columns; a tool that hands us a flat
 * catalogue row does not, so the spec columns are whatever is left after these are removed.
 */
export const META_KEYS = new Set([
  "mpn", "manufacturer", "specs", "params", "params_full", "notes", "status",
  "grade", "penalty", "direction", "footprint", "margins", "sortKey", "evidence",
  "envelope", "line", "lineno", "srcOffset", "srcLength", "original_unverified",
  "row", "height_fit", "missing_dimensions", "reason", "filled", "category", "ref_des",
]);

/** Numbers whose only useful reading is a ratio, not a magnitude. */
const MARGIN_LIMIT = 4;
export const SPEC_LIMIT = 9;

/**
 * The spec object for a row.
 *
 * In priority order: what the server projected (`specs`), then the row's own flat scalars,
 * then a nested `row` if that is where the catalogue datum ended up. The last case is a
 * fallback for payloads this widget does not own — Kelvin's cross_reference now projects
 * `specs` in the RANKER's vocabulary, which is the one the original's specs are in, so the
 * two sides of the comparison line up. Reading the nested row instead would tabulate
 * `vds_rated` under a header row that says `vds`.
 */
export function specsOf(row) {
  // Kelvin now always projects `specs` (the pipeline result contract, ABT #685), so that is
  // the first and normally the only branch. A projected specs object still goes through the
  // metadata filter: the ranker's spec carries `mpn` on purpose (the AEC-Q and rated-voltage
  // gates decode it), and it must not become a column repeating the identity cell.
  if (row?.specs && typeof row.specs === "object") return scalars(row.specs);
  // The remaining branches serve payloads this widget does not own — a server that sends a
  // flat catalogue row, or one that nests it. Kept because the widget is meant to be generic
  // (ABT #663), not because Kelvin needs them.
  const flat = scalars(row);
  if (Object.keys(flat).length) return flat;
  if (row?.row && typeof row.row === "object") return scalars(row.row);
  return {};
}

function scalars(obj) {
  const out = {};
  for (const [k, v] of Object.entries(obj ?? {})) {
    if (META_KEYS.has(k)) continue;
    // A leading underscore marks a field the pipeline carries for its own use, not a
    // datasheet parameter — the ranker's collision-proof row key `_key`
    // ("STMicroelectronics<US>STP60NF06") is one, and the browse path will add others.
    if (k.startsWith("_")) continue;
    if (v === null || v === undefined || typeof v === "object") continue;
    out[k] = v;
  }
  return out;
}

/**
 * Fields that describe the PACKAGE rather than the part's behaviour.
 *
 * They belong in the table — a substitute that does not fit is not a substitute — but behind
 * the electrical parameters. The ranker's spec object leads with them (it spreads base() first),
 * so without this the column cap spends its budget on case code and mount and truncates
 * Rds(on) and Vgs(max), which is the same "cannot compare candidates" complaint in a quieter
 * form. Presentation order only: nothing is dropped that would otherwise be shown.
 */
const PACKAGING_KEYS = new Set([
  "case_code", "caseCode", "mount", "is_production", "qualification",
  "length_m", "width_m", "height_m", "lengthM", "widthM", "heightM",
  "temp_min_c", "temp_max_c", "temp_min_C", "temp_max_C",
]);

/** Union of spec keys across rows, so the table has stable columns. */
export function specColumns(rows) {
  const seen = [];
  for (const r of rows) {
    for (const k of Object.keys(specsOf(r))) if (!seen.includes(k)) seen.push(k);
  }
  const ordered = [...seen.filter((k) => !PACKAGING_KEYS.has(k)),
                   ...seen.filter((k) => PACKAGING_KEYS.has(k))];
  return ordered.slice(0, SPEC_LIMIT);       // a catalogue row can carry dozens
}

/**
 * Margin columns — the recommender's comparison axis.
 *
 * A selector's candidates carry no absolute specs: the engine returns what it ranked on, which
 * is HEADROOM against the requirement the caller supplied (vds_margin 1.0 = exactly at the
 * limit; rds_on_headroom 153 = two orders of margin). The web app's own recommend view is
 * built out of these as meters for the same reason, so the widget shows them rather than a
 * bare list of part numbers. Absent stays absent: a null margin is a datum the record could
 * not supply, and giving it a column of dashes is more honest than dropping the candidate's
 * only distinguishing number.
 */
export function marginColumns(rows) {
  const seen = [];
  for (const r of rows) {
    for (const [k, v] of Object.entries(r?.margins ?? {})) {
      if (typeof v === "number" && !seen.includes(k)) seen.push(k);
    }
  }
  return seen.slice(0, MARGIN_LIMIT);
}

/**
 * The metric a selector ranked by, if every candidate agrees on one.
 *
 * `sortKey` is {metric, value} — the number that actually decided the order. It earns a column
 * because without it the ranking is unexplained: the list is sorted by something the reader
 * cannot see.
 */
export function rankColumn(rows) {
  const metrics = new Set(rows.map((r) => r?.sortKey?.metric).filter(Boolean));
  return metrics.size === 1 ? [...metrics][0] : null;
}

/**
 * Every column the table will draw, in order, as {key, label, kind}.
 *
 * kind drives formatting only: "spec" is a magnitude, "margin" a ratio (rendered ×N), "rank"
 * the ranking metric's own value.
 */
export function columnsFor(rows) {
  const cols = specColumns(rows).map((key) => ({ key, label: key, kind: "spec" }));
  const metric = rankColumn(rows);
  if (metric) cols.push({ key: "__rank", label: metric.replace(/_/g, " "), kind: "rank" });
  for (const key of marginColumns(rows)) {
    cols.push({ key, label: key.replace(/_/g, " "), kind: "margin" });
  }
  return cols;
}

/** The value a column reads out of one candidate. */
export function valueFor(row, col) {
  if (col.kind === "spec") return specsOf(row)[col.key];
  if (col.kind === "rank") return row?.sortKey?.value;
  return row?.margins?.[col.key];
}
