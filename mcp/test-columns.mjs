// What the picker will actually draw, checked against REAL tool payloads.
//
// The widget module connects to the MCP Apps host at import, so its rendering could only ever
// be checked by driving the GUI — and that is how both ranked views shipped deriving a single
// column each while the browse view derived ten. The column logic now lives in src/columns.js,
// and this runs it over payloads captured from the live server.
//
//   node test-columns.mjs            # uses the recorded payloads below
//
// Payload fixtures are the shapes the server emits today (verified against a live run), which
// since ABT #685 are the Moebius pipeline-result contract: one candidate type, parameters under
// `specs`, pipeline-internal fields underscored. search_parts -> catalogue specs;
// recommend_parts -> margins + sortKey and NO specs (a selector returns what it RANKED on);
// cross_reference -> ranker verdict + `specs` in the ranker's vocabulary + the raw `row`.

import { columnsFor, specsOf, valueFor } from "./src/columns.js";

let failures = 0;
const check = (label, cond, detail = "") => {
  console.log(`  ${cond ? "ok  " : "FAIL"}  ${label}${detail ? ` — ${detail}` : ""}`);
  if (!cond) failures++;
};

// ── fixtures ────────────────────────────────────────────────────────────────
const SEARCH_ROW = {
  mpn: "890324023006", manufacturer: "Würth Elektronik",
  _lineno: 12, _srcOffset: 1, _srcLength: 2,
  specs: {
    capacitance: 1e-7, v_rated: 630, ripple_current_rms: 1.5,
    temp_min_c: -55, temp_max_c: 105, technology: "film-polypropylene", dielectric_code: "",
    is_production: true, lengthM: 0.018, widthM: 0.009, heightM: 0.0185, mount: "THT",
  },
};

const RECOMMEND_CANDIDATE = {
  mpn: "CSD18536KCS", manufacturer: "Texas Instruments", _line: 287,
  _srcOffset: 373462, _srcLength: 1263,
  evidence: { datasheetUsable: true, qgPresent: true, thermalPresent: true },
  margins: { id_margin: 40.0, qg_headroom: null, rds_on_headroom: 153.84, vds_margin: 1.0 },
  sortKey: { metric: "lowest_rds_on", value: 0.00065 },
};

const CROSSREF_CANDIDATE = {
  _key: "STMicroelectronics␟STP60NF06", mpn: "STP60NF06", manufacturer: "STMicroelectronics",
  status: "recommended", grade: "drop_in", direction: "upgrade", penalty: 0.136,
  footprint: "same_case",
  params: [{ name: "rds_on", verdict: "pass" }, { name: "qg", verdict: "pass" }],
  specs: {                                   // the ranker's vocabulary — matches originalSpecs
    mpn: "STP60NF06", vds: 60, id: 60, rds_on: 0.014, qg: 5.8e-8, coss: 5.2e-10,
    vgs_threshold_max: 4, technology: "Si", mount: "THT",
    length_m: 0.0105, width_m: 0.0046, height_m: 0.0154, case_code: "TO-220",
    is_production: true, qualification: "",
  },
  row: {                                     // the shard's vocabulary — deliberately NOT used
    mpn: "STP60NF06", manufacturer: "STMicroelectronics", vds_rated: 60, id_continuous: 60,
    rds_on: 0.014, qg_total: 5.8e-8, lineno: 55, srcOffset: 9, srcLength: 9,
  },
};

// ── the bug this file exists for ────────────────────────────────────────────
console.log("search_parts (catalogue specs under `specs`)");
{
  const cols = columnsFor([SEARCH_ROW, { ...SEARCH_ROW, mpn: "X" }]);
  const keys = cols.map((c) => c.key);
  check("derives spec columns", cols.length >= 5, keys.join(", "));
  check("identity and locators are not columns",
    !keys.some((k) => ["mpn", "manufacturer", "srcOffset", "srcLength", "lineno"].includes(k)));
  check("reads a real value", valueFor(SEARCH_ROW, cols.find((c) => c.key === "capacitance")) === 1e-7);
  check("underscored locators never become columns",
    !keys.some((k) => k.startsWith("_")), keys.join(", "));
}

console.log("cross_reference (ranker verdict + projected specs)");
{
  const cols = columnsFor([CROSSREF_CANDIDATE]);
  const keys = cols.map((c) => c.key);
  check("derives spec columns rather than a bare part list", cols.length >= 5, keys.join(", "));
  check("uses the RANKER's vocabulary, so candidates line up with the original's specs",
    keys.includes("vds") && keys.includes("id") && !keys.includes("vds_rated"),
    keys.slice(0, 6).join(", "));
  check("the internal row key never becomes a column", !keys.includes("_key"));
  check("the nested raw row never becomes a column", !keys.includes("row"));
  check("the part number is not repeated as a spec column", !keys.includes("mpn"),
    "the ranker's spec carries mpn on purpose — the MPN decode gates need it");
  check("reads a real value", valueFor(CROSSREF_CANDIDATE, cols.find((c) => c.key === "vds")) === 60);
}

console.log("recommend_parts (no specs exist — margins ARE the comparison axis)");
{
  const cols = columnsFor([RECOMMEND_CANDIDATE, { ...RECOMMEND_CANDIDATE, mpn: "Y" }]);
  const keys = cols.map((c) => c.key);
  check("does not render as a bare list of part numbers", cols.length >= 3, keys.join(", "));
  check("the ranking metric gets a column", keys.includes("__rank"),
    cols.find((c) => c.kind === "rank")?.label);
  check("every numeric margin gets a column",
    ["vds_margin", "id_margin", "rds_on_headroom"].every((k) => keys.includes(k)));
  check("a null margin is not given a column of its own",
    !keys.includes("qg_headroom"), "qg_headroom is null on every candidate");
  check("reads the ranking value",
    valueFor(RECOMMEND_CANDIDATE, cols.find((c) => c.kind === "rank")) === 0.00065);
  check("reads a margin",
    valueFor(RECOMMEND_CANDIDATE, cols.find((c) => c.key === "vds_margin")) === 1.0);
}

console.log("specsOf priority");
{
  check("a server-projected specs object wins",
    specsOf({ specs: { a: 1 }, b: 2 }).a === 1 && specsOf({ specs: { a: 1 }, b: 2 }).b === undefined);
  check("flat scalars are used when there is no specs object", specsOf({ b: 2 }).b === 2);
  check("a nested row is the last resort, not the first",
    specsOf({ row: { c: 3 } }).c === 3 && specsOf({ specs: { a: 1 }, row: { c: 3 } }).c === undefined);
  check("nothing to show is an empty object, not a crash",
    Object.keys(specsOf({ mpn: "X", manufacturer: "Y" })).length === 0);
  check("null and undefined rows do not throw",
    Object.keys(specsOf(null)).length === 0 && Object.keys(specsOf(undefined)).length === 0);
}

console.log();
if (failures) {
  console.log(`${failures} FAILED`);
  process.exit(1);
}
console.log("all column checks passed");
