"""End-to-end smoke test for the Kelvin MCP server — every tool, against the real catalogue.

Not a unit test: it calls the tools the way a host does (through the FastMCP tool registry, so
the registered schema and the function must agree), over the actual TAS NDJSON, and asserts the
answers are the catalogue's rather than empty. The point is that a broken tool fails HERE, not
in a chat session where "no candidates" and "nobody looked" read the same.

    KELVIN_TAS_DATA_DIR=/path/to/catalogue python3 mcp/smoke.py [--skip-xref]

--skip-xref leaves out cross_reference (the only tool needing Node + prebuilt shards).
"""

from __future__ import annotations

import asyncio
import sys

import server as S

SKIP_XREF = "--skip-xref" in sys.argv
FAILURES: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {label}" + (f" — {detail}" if detail else ""))
    if not condition:
        FAILURES.append(label)


def text(result) -> str:
    return "\n".join(c.text for c in result.content)


def main() -> int:
    print("list_families")
    r = S.list_families()
    names = [f["family"] for f in r.structuredContent["families"]]
    check("all twelve families listed", len(names) == 12, ", ".join(names))
    check("browse-only families flagged",
          all(f["selector"] is False for f in r.structuredContent["families"]
              if f["family"] in S.BROWSE_ONLY))

    print("describe_family(capacitor)")
    r = S.describe_family("capacitors")            # plural must normalise
    fields = r.structuredContent["fields"]
    check("capacitance is filterable", "capacitance" in fields["numeric"])
    check("technology is facetable", "technology" in fields["string"])
    check("part count reported", r.structuredContent["rows"] > 1000,
          f"{r.structuredContent['rows']:,} parts")

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
    page = r.structuredContent
    rows = page["candidates"]                      # the shared ranked-list envelope
    check("matches found", page["total"] > 0, f"{page['total']:,} parts")
    check("page respects the limit", len(rows) == 5)
    check("every row is inside the window",
          all(100e-9 <= row["capacitance"] <= 470e-9 and row["v_rated"] >= 300 for row in rows))
    check("sorted ascending", rows == sorted(rows, key=lambda row: row["capacitance"]))
    check("facets counted", bool((page.get("facets") or {}).get("technology", {}).get("values")))
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
    r = S.part_details("capacitor", mpn, full_record=True)
    payload = r.structuredContent
    check("the row came back", payload["row"]["mpn"] == mpn)
    check("the full TAS record came back", "capacitor" in (payload.get("record") or {}))
    check("record is THAT part",
          payload["record"]["capacitor"]["manufacturerInfo"]["reference"] == mpn)

    print("part_details for a part that is not there")
    try:
        S.part_details("capacitor", "NOT-A-REAL-MPN-XYZ")
        check("missing part refused", False)
    except ValueError as e:
        check("missing part refused loudly", "no capacitor" in str(e))

    print("recommend_parts(mosfet 60 V / 5 A / 100 mΩ)")
    r = S.recommend_parts("mosfet", {"ratedDrainSourceVoltage": 60,
                                     "ratedContinuousDrainCurrent": 5,
                                     "maximumOnResistance": 0.1}, max_candidates=5)
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
    hist = r.structuredContent["histogram"]
    check("buckets counted", sum(hist["counts"]) == hist["present"] > 0,
          f"{hist['present']:,} parts state Rds(on)")
    check("absences are reported, not dropped", "absent" in hist)

    if SKIP_XREF:
        print("cross_reference: SKIPPED (--skip-xref)")
    else:
        print("cross_reference(capacitor)")
        r = S.cross_reference("capacitor", mpn, max_results=5)
        x = r.structuredContent
        check("the original resolved", x["original"]["mpn"] == mpn)
        check("a candidate pool was scored", x["poolScored"] > 0,
              f"{x['poolScored']:,} of {x['poolTotal']:,}")
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
        cand_specs = set((x["candidates"][0].get("specs") or {}))
        orig_specs = set(x.get("originalSpecs") or {})
        check("candidate specs are the ranker's own projection, not the shard row",
              bool(cand_specs) and len(cand_specs - orig_specs) == 0,
              f"{len(cand_specs & orig_specs)}/{len(cand_specs)} keys align"
              + (f"; stray: {sorted(cand_specs - orig_specs)[:4]}" if cand_specs - orig_specs else ""))
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

    print("the registered tool surface")
    tools = asyncio.run(S.mcp.list_tools())
    check("every tool is registered", len(tools) == 7, ", ".join(t.name for t in tools))
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
    with_ui = {t.name for t in tools if (t.meta or {}).get("ui/resourceUri")}
    check("the picker is on exactly the ranked-list tools",
          with_ui == {"search_parts", "recommend_parts", "cross_reference"},
          ", ".join(sorted(with_ui)))

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: " + "; ".join(FAILURES))
        return 1
    print("all smoke checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
