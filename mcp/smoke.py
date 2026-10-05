"""End-to-end smoke test for the Kelvin MCP server — every tool, against the real catalogue.

Not a unit test: it calls the tools the way a host does (through the FastMCP tool registry, so
the registered schema and the function must agree), over the actual TAS NDJSON, and asserts the
answers are the catalogue's rather than empty. The point is that a broken tool fails HERE, not
in a chat session where "no candidates" and "nobody looked" read the same.

    KELVIN_TAS_DATA_DIR=/path/to/catalogue python3 mcp/smoke.py [--skip-xref]

--skip-xref leaves out cross_reference and crossref_bom (the tools needing Node + prebuilt shards).
The BOM-file parser has its own catalogue-free tests: python3 -m pytest mcp/test_bomfile.py
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
from pathlib import Path

import server as S

SKIP_XREF = "--skip-xref" in sys.argv
FAILURES: list[str] = []


# Moebius validates every payload at its boundary against this schema and rejects anything
# that does not conform (ABT #685), so the schema is checked HERE against the same file rather
# than against a local copy of what we think it says. Absent, it is skipped loudly.
CONTRACT = Path(os.environ.get(
    "MOEBIUS_CONTRACT",
    "/home/alf/wuerth/moebius-orchestrator/contracts/pipeline_result.json"))
_validator = None


def conforms(label: str, payload: dict) -> None:
    """Assert a tool result against the pipeline contract."""
    global _validator
    if _validator is None:
        if not CONTRACT.exists():
            check(f"contract check for {label}", False, f"schema not found at {CONTRACT}")
            return
        import jsonschema

        _validator = jsonschema.Draft202012Validator(json.loads(CONTRACT.read_text()))
    errors = sorted(_validator.iter_errors(payload), key=lambda e: list(e.path))
    detail = ""
    if errors:
        first = errors[0]
        detail = f"{'/'.join(str(p) for p in first.path) or '(root)'}: {first.message[:120]}"
    check(f"{label} conforms to the pipeline contract", not errors, detail)


def check(label: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {label}" + (f" — {detail}" if detail else ""))
    if not condition:
        FAILURES.append(label)


def text(result) -> str:
    return "\n".join(c.text for c in result.content)


BOM_FIXTURES = Path(__file__).parent / "fixtures" / "bom"
# A BOM payload carries every line, so its size grows with the BOM. The bound is per line:
# Faraday's board version came back at 691,751 characters for 189 parts (~3,700 a line) before
# it was compacted, and clients refuse a result that size outright.
BOM_CHARS_PER_LINE = 800
BOM_CHARS_ONE_LINE = 1500


def _bom(path, **kw):
    r = S.crossref_bom(str(path), **kw)
    return r, r.structuredContent, {line["ref"]: line for line in r.structuredContent["lines"]}


def _raises(label: str, fn, *needles: str) -> None:
    try:
        fn()
        check(label, False, "no exception")
    except ValueError as e:
        check(label, all(n in str(e) for n in needles), str(e)[:160])


def bom_checks() -> None:
    import tempfile

    print("crossref_bom(Altium CSV, Windows-1252, title block, grouped designators)")
    r, p, by = _bom(BOM_FIXTURES / "altium_bom.csv")
    conforms("crossref_bom (Altium)", p)
    check("one line per designator", p["total"] == len(p["lines"]) == 14, f"{p['total']} lines")
    check("grouped designators expand, each its own line",
          {"R3", "R4", "R5", "C1", "C2", "C3", "C4", "D1", "D2"} <= set(by))
    check("a designator sharing its row's answer points at the first",
          by["R4"].get("_sameAs") == "R3" and by["R4"]["status"] == by["R3"]["status"]
          and by["R4"]["mpn"] == by["R3"]["mpn"])
    r1 = by["R1"]
    check("a catalogued MPN is identified exactly",
          r1["_identification"]["certainty"] == "exact" and r1["originalMpn"] == "RC0402FR-132K2L"
          and r1["kind"] == "resistor" and "identification: exact" in r1["notes"])
    check("... and cross-referenced by the ranker",
          bool(r1.get("candidates")) and r1["candidates"][0].get("status") is not None
          and r1["status"] in ("recommended", "partial", "no_substitute"),
          f"{r1['status']} -> {r1['mpn']}")
    check("every substitute is from another vendor when no target is named",
          all(c.get("manufacturer") != "YAGEO" for c in r1["candidates"]))
    u1 = by["U1"]
    check("an MPN the catalogue lacks is unsourced, with its reason",
          u1["status"] == "unsourced" and u1["mpn"] is None
          and u1["_identification"]["certainty"] == "none"
          and "no catalogue part carries 'SN74LVC1T45DCKR'" in u1["notes"], u1["notes"][:120])
    c5 = by["C5"]
    check("an unknown MPN with a value and package lists what it might be, unranked",
          c5["status"] == "unsourced" and c5["_identification"]["certainty"] == "value-package"
          and "TMK105BJ104KV-F" in c5["notes"]
          and all("status" not in c for c in c5.get("candidates") or []),
          c5["notes"][:120])
    check("the Windows-1252 read is in the payload caveat, not only in the digest",
          "Windows-1252" in p["caveat"])
    check("unread columns and skipped rows are reported",
          any("'LibRef'" in d for d in p.get("diagnostics") or [])
          and any("1 totals" in d for d in p.get("diagnostics") or []))
    size = len(json.dumps(p, ensure_ascii=False))
    biggest = max(len(json.dumps(line, ensure_ascii=False)) for line in p["lines"])
    check(f"the payload stays under {BOM_CHARS_PER_LINE} characters a line",
          size <= BOM_CHARS_PER_LINE * p["total"] and biggest <= BOM_CHARS_ONE_LINE,
          f"{size:,} chars for {p['total']} lines, biggest line {biggest:,}")

    print("crossref_bom_line(the handle crossref_bom returned)")
    handle = p["caveat"].split("crossref='")[1][:12]
    line = S.crossref_bom_line(handle, "R1").structuredContent
    conforms("crossref_bom_line", line)
    full = line["lines"][0]
    check("the stored line carries every ranked candidate with its spec table",
          len(full["candidates"]) > len(r1["candidates"])
          and all("specs" in c for c in full["candidates"]))
    check("the stored line agrees with the compact one",
          full["status"] == r1["status"] and full["mpn"] == r1["mpn"])
    _raises("an unknown ref is refused, naming what the handle holds",
            lambda: S.crossref_bom_line(handle, "Q99"), "no line 'Q99'", "R1")

    print("crossref_bom(Altium CSV) into Würth, spelled without the umlaut")
    r, p, by = _bom(BOM_FIXTURES / "altium_bom.csv", target_manufacturers=["wurth"])
    conforms("crossref_bom (Würth target)", p)
    check("a part already from the target maker is 'exact', itself",
          by["C1"]["status"] == "exact" and by["C1"]["mpn"] == "885012206095"
          and by["L1"]["status"] == "exact")
    check("every substitute offered is the target maker's",
          all("rth" in (line.get("manufacturer") or "")
              for line in p["lines"] if line["mpn"]),
          ", ".join(sorted({line.get("manufacturer") or "" for line in p["lines"] if line["mpn"]})))
    check("targetManufacturer names the single target", p.get("targetManufacturer") == "wurth")

    print("crossref_bom(KiCad CSV)")
    r, p, by = _bom(BOM_FIXTURES / "kicad_bom.csv", target_manufacturers=["Würth Elektronik"])
    conforms("crossref_bom (KiCad)", p)
    check("a value-only line is matched by value and package",
          by["C1"]["_identification"]["certainty"] == "value-package"
          and by["C1"]["kind"] == "capacitor")
    check("the target maker's parts of that value are listed first",
          (by["C1"].get("candidates") or [{}])[0].get("manufacturer") == "Würth Elektronik")

    print("crossref_bom(tab-separated .txt, via file://)")
    r, p, by = _bom(f"file://{BOM_FIXTURES / 'tab_bom.txt'}",
                    target_manufacturers=["WURTH", "Nobody Inc"])
    conforms("crossref_bom (TSV)", p)
    check("a manufacturer that makes nothing is said, once",
          any("'Nobody Inc'" in d for d in p.get("diagnostics") or []))
    check("the diode is identified and cross-referenced",
          by["D1"]["_identification"]["certainty"] == "exact" and by["D1"]["kind"] == "diode")

    with tempfile.TemporaryDirectory() as tmp:
        print("crossref_bom(.xlsx written here)")
        import openpyxl

        book = openpyxl.Workbook()
        sheet = book.active
        sheet.title = "BOM"
        sheet.append(["Designator", "Qty", "Manufacturer", "MPN"])
        sheet.append(["C1-C2", 2, "Würth Elektronik", 885012106006])
        sheet.append(["R1", 1, "YAGEO", "RC0402FR-1310KL"])
        xlsx = Path(tmp) / "bom.xlsx"
        book.save(xlsx)
        r, p, by = _bom(xlsx)
        conforms("crossref_bom (xlsx)", p)
        check("a numeric MPN cell is read as the ordering code",
              by["C1"]["_identification"]["certainty"] == "exact"
              and by["C1"]["originalMpn"] == "885012106006")

        print("crossref_bom on a family Kelvin cannot cross-reference")
        page = S._browse("controller", {"limit": 1, "sort": {"field": "lineno", "dir": "asc"}})
        ctrl = page["rows"][0]
        path = Path(tmp) / "ctrl.csv"
        path.write_text(f"Designator,MPN\nU7,{ctrl['mpn']}\n", encoding="utf-8")
        r, p, by = _bom(path)
        conforms("crossref_bom (controller)", p)
        check("an identified part with no cross-reference model is refused, not ranked",
              by["U7"]["status"] == "unsourced"
              and by["U7"]["_identification"]["certainty"] == "exact"
              and "no cross-reference model" in by["U7"]["notes"], by["U7"]["notes"][:140])

        print("crossref_bom refusals")
        bad = Path(tmp) / "qty.csv"
        bad.write_text("Designator,Quantity,MPN\nR1-R3,2,RC0402FR-1310KL\n", encoding="utf-8")
        _raises("a quantity mismatch is refused, naming the row",
                lambda: S.crossref_bom(str(bad)), "row 2", "Quantity is 2")
        nohead = Path(tmp) / "nohead.csv"
        # One column: nothing for Jev to map either (a header Kelvin does not know but with
        # two or more columns goes to Jev — test_bomfile.py covers that, mocked and live).
        nohead.write_text("Parts\nRC0402FR-1310KL\n", encoding="utf-8")
        _raises("no recognisable header is refused, listing what is there",
                lambda: S.crossref_bom(str(nohead)), "no row names the BOM's columns",
                "Parts")
        _raises("a missing file is refused by path",
                lambda: S.crossref_bom(str(Path(tmp) / "absent.csv")), "no BOM at")


def main() -> int:
    print("list_families")
    r = S.list_families()
    conforms("list_families", r.structuredContent)
    names = [f["family"] for f in r.structuredContent["families"]]
    check("all twelve families listed", len(names) == 12, ", ".join(names))
    check("browse-only families flagged",
          all(f["selector"] is False for f in r.structuredContent["families"]
              if f["family"] in S.BROWSE_ONLY))

    print("describe_family(capacitor)")
    r = S.describe_family("capacitors")            # plural must normalise
    conforms("describe_family", r.structuredContent)
    fields = r.structuredContent["fields"]
    check("capacitance is filterable", "capacitance" in fields["numeric"])
    check("technology is facetable", "technology" in fields["categorical"])
    check("part count reported", r.structuredContent["catalogueTotal"] > 1000,
          f"{r.structuredContent['catalogueTotal']:,} parts")

    print("describe_family(nonsense)")
    try:
        S.describe_family("flux_capacitor")
        check("unknown family rejected", False)
    except ValueError as e:
        check("unknown family rejected", "unknown family" in str(e))

    print("search_parts(capacitor, 100n..470n, >=300 V)")
    r = S.search_parts("capacitor",
                       filters={"capacitance": {"min": 100e-9, "max": 470e-9},
                                "v_rated": {"min": 300}},
                       sort={"field": "capacitance", "dir": "asc"}, limit=5, with_facets=True)
    conforms("search_parts", r.structuredContent)
    page = r.structuredContent
    rows = page["candidates"]                      # the shared ranked-list envelope
    check("matches found", page["total"] > 0, f"{page['total']:,} parts")
    check("page respects the limit", len(rows) == 5)
    check("every row is inside the window",
          all(100e-9 <= c["specs"]["capacitance"] <= 470e-9 and c["specs"]["v_rated"] >= 300
              for c in rows))
    check("sorted ascending",
          rows == sorted(rows, key=lambda c: c["specs"]["capacitance"]))
    check("parameters live under specs, never flat on the candidate",
          all("capacitance" not in c and "specs" in c for c in rows))
    check("locators travel underscored, as pipeline-internal",
          all("_srcOffset" in c and "srcOffset" not in c for c in rows))
    check("the family size and the match count are separate, named facts",
          page["catalogueTotal"] > page["total"] > 0,
          f"{page['total']:,} matched of {page['catalogueTotal']:,}")
    # The contract has no facet field, so the counts must reach the reader in the digest
    # rather than being dropped for want of somewhere to put them.
    check("facet values and counts are reported in the digest",
          "film-polypropylene" in text(r) and "technology:" in text(r))
    check("digest names real parts", rows[0]["mpn"] in text(r))

    print("search_parts with a field that does not exist")
    try:
        S.search_parts("capacitor", filters={"esr_at_100khz": {"min": 1}})
        check("unknown filter field rejected", False)
    except ValueError as e:
        check("unknown filter field rejected and the vocabulary named",
              "esr_at_100khz" in str(e) and "capacitance" in str(e))

    print("part_details")
    mpn = rows[0]["mpn"]
    r = S.part_details("capacitor", mpn, include_record=True)
    payload = r.structuredContent
    conforms("part_details", payload)
    check("the part came back", payload["part"]["mpn"] == mpn)
    check("the full TAS record came back", "capacitor" in (payload["part"].get("record") or {}))
    check("record is THAT part",
          payload["part"]["record"]["capacitor"]["manufacturerInfo"]["reference"] == mpn)

    print("part_details for a part that is not there")
    try:
        S.part_details("capacitor", "NOT-A-REAL-MPN-XYZ")
        check("missing part refused", False)
    except ValueError as e:
        check("missing part refused loudly", "no capacitor" in str(e))

    print("recommend_parts(mosfet 60 V / 5 A / 100 mΩ)")
    r = S.recommend_parts("mosfet", {"ratedDrainSourceVoltage": 60,
                                     "ratedContinuousDrainCurrent": 5,
                                     "maximumOnResistance": 0.1}, max_results=5)
    conforms("recommend_parts", r.structuredContent)
    cands = r.structuredContent["candidates"]
    check("candidates returned", len(cands) > 0, f"best: {cands[0]['mpn']}")
    # Margins are ratios of the part's rating to the requirement, so clearing a gate means >= 1.
    check("every candidate clears every gate",
          all(c["margins"]["vds_margin"] >= 1.0 and c["margins"]["id_margin"] >= 1.0
              and c["margins"]["rds_on_headroom"] >= 1.0 for c in cands),
          f"tightest vds_margin {min(c['margins']['vds_margin'] for c in cands):.2f}x")
    check("no envelope unless asked", "envelope" not in cands[0])

    print("recommend_parts with an unsatisfiable requirement")
    try:
        S.recommend_parts("mosfet", {"ratedDrainSourceVoltage": 1e9,
                                     "ratedContinuousDrainCurrent": 1e9,
                                     "maximumOnResistance": 1e-12})
        check("impossible requirement refused", False)
    except ValueError as e:
        check("impossible requirement names the gate that rejected",
              "Rejections by gate" in str(e) and "=" in str(e))

    print("recommend_parts on a browse-only family")
    try:
        S.recommend_parts("timing", {"frequency": 16e6})
        check("browse-only family refused", False)
    except ValueError as e:
        check("browse-only family refused", "no selector" in str(e))

    print("spec_distribution(mosfet rds_on over 100 V silicon)")
    r = S.spec_distribution("mosfet", "rds_on", filters={"vds_rated": {"min": 100}}, buckets=12)
    conforms("spec_distribution", r.structuredContent)
    dist = r.structuredContent
    hist = dist["histogram"]
    check("buckets counted", sum(hist["counts"]) == dist["present"] > 0,
          f"{dist['present']:,} parts state Rds(on)")
    check("absences are reported, not dropped", "absent" in dist)

    if SKIP_XREF:
        print("cross_reference: SKIPPED (--skip-xref)")
    else:
        print("cross_reference(capacitor)")
        r = S.cross_reference("capacitor", mpn, max_results=5)
        x = r.structuredContent
        conforms("cross_reference", x)
        check("the original resolved", x["original"]["mpn"] == mpn)
        check("a candidate pool was scored", x["total"] > 0, f"{x['total']:,} pre-gated")
        check("substitutes ranked best-first",
              [c["penalty"] for c in x["candidates"]]
              == sorted(c["penalty"] for c in x["candidates"]))
        check("every substitute is from another vendor",
              all(c["manufacturer"] != x["original"]["manufacturer"] for c in x["candidates"]))
        check("the digest carries the ranker's reasoning, not just names and scores",
              any(line.strip().startswith(("concerns:", "note:")) for line in text(r).splitlines()))
        # The widget tabulates candidates directly under the original's specs, so the two
        # must be in ONE vocabulary. Overlap is not enough: the shard row and the ranker's
        # spec share `rds_on` and `coss` by coincidence, which is exactly what made a wholly
        # mismatched table look healthy at a glance.
        # Compared against the worker's RAW projection, not the payload's originalSpecs: the
        # payload omits keys the original does not state (a null is not sent), so a candidate
        # that states ripple_current where the original does not is correct, not a vocabulary
        # break. The invariant is that both sides come from the same projection function.
        raw = S._xref({"op": "crossref", "family": "capacitor", "mpn": mpn, "maxResults": 1})
        vocabulary = set(raw["origSpec"]) - {"_key"}
        cand_specs = set((x["candidates"][0].get("specs") or {}))
        check("candidate specs are the ranker's own projection, not the shard row",
              bool(cand_specs) and cand_specs <= vocabulary,
              f"{len(cand_specs)} keys, all in the ranker's vocabulary"
              if cand_specs <= vocabulary else f"stray: {sorted(cand_specs - vocabulary)[:4]}")
        check("the original's stated specs are a subset of that same vocabulary",
              set(x.get("originalSpecs") or {}) <= vocabulary)
        check("the shard vocabulary never reaches the comparison",
              not ({"vds_rated", "id_continuous", "qg_total"} & cand_specs))

        print("the cross-reference worker restarts when its source changes")
        S._xref({"op": "families"})                       # ensure a worker is up
        first = S._xref_proc.pid
        stamp = S._XREF_SOURCES[0]
        original = stamp.read_bytes()
        try:
            stamp.write_bytes(original + b"\n// staleness probe\n")
            S._xref({"op": "families"})
            check("a source edit restarts the worker instead of serving the old code",
                  S._xref_proc.pid != first, f"pid {first} -> {S._xref_proc.pid}")
        finally:
            stamp.write_bytes(original)
        S._xref({"op": "families"})                       # back to the real source

        print("cross_reference on a family with no substitute model")
        try:
            S.cross_reference("controller", "UCC28180")
            check("unmodelled family refused", False)
        except ValueError as e:
            check("unmodelled family refused", "no cross-reference model" in str(e))

    if SKIP_XREF:
        print("crossref_bom: SKIPPED (--skip-xref)")
    else:
        bom_checks()

    print("the registered tool surface")
    tools = asyncio.run(S.mcp.list_tools())
    check("every tool is registered", len(tools) == 9, ", ".join(t.name for t in tools))
    check("every tool has a description", all(t.description for t in tools))

    print("the MCP Apps widget")
    S.assert_widgets_resolve()
    check("every advertised ui:// has a bundle behind it", True,
          ", ".join(S.UI_BUNDLES))
    widget = S.picker_widget()
    check("the bundle is self-contained HTML",
          widget.lstrip().startswith("<") and "<script" in widget, f"{len(widget):,} bytes")
    check("no external fetch in the widget (it renders under a deny-by-default CSP)",
          "src=\"http" not in widget and "src='http" not in widget)
    # The three ranked-list tools carry the picker; the other four must not advertise
    # a UI they do not fill.
    with_ui = {t.name: t.meta["ui/resourceUri"] for t in tools
               if (t.meta or {}).get("ui/resourceUri")}
    picker, table = "ui://kelvin/picker.html", "ui://kelvin/crossref-table.html"
    check("the picker is on exactly the ranked-list tools, the cross-reference table on "
          "crossref_bom",
          with_ui == {"search_parts": picker, "recommend_parts": picker,
                      "cross_reference": picker, "crossref_bom": table},
          ", ".join(f"{k}->{v}" for k, v in sorted(with_ui.items())))
    table_html = S.crossref_widget()
    check("the cross-reference table is self-contained HTML with no external fetch",
          table_html.lstrip().startswith("<") and "<script" in table_html
          and 'src="http' not in table_html and "src='http" not in table_html,
          f"{len(table_html):,} bytes")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: " + "; ".join(FAILURES))
        return 1
    print("all smoke checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
