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
