#!/usr/bin/env python3
"""Gate coverage — which catalogue files is nothing checking?

WHY. A data file with no gate mapping does not fail; it is simply never examined, and
"no findings" from it is indistinguishable from "clean". data/circuits.ndjson sat in that
state for its entire existence: changed_records_gate.py had no FAMILIES entry for it, so
it exited 2 ("NO FAMILY MAPPING — refusing to pass") on any change touching it, and the
13,615 CIAS bricks inside were checked by nothing but a schema pass. The first real gate
run over them found 50 pre-existing defects.

That hole was invisible because every individual run behaved correctly. Nothing was
looking at the SET of files versus the SET of things gated. This does.

It also reports the reverse — a gate mapping for a file that no longer exists — because a
stale mapping is how a gate ends up silently skipping.

Exit codes: 0 fully covered, 1 a gap, 2 the audit could not run (which is not a pass).

  python3 gate_coverage.py                 # audit TAS
  python3 gate_coverage.py --json          # machine-readable, for the loop
"""
import argparse
import json
import re
import sys
from pathlib import Path

TAS = Path.home() / "PSMA" / "TAS"

# Files that legitimately have no per-record gate, each with the reason. Anything NOT
# here and not in the gate's tables is a hole, not an exemption — the list is explicit so
# that adding to it is a visible decision rather than a silent skip.
EXEMPT = {
    # Quarantine, in all the spellings the repo has accumulated: the modern
    # "<family>.quarantine_<reason>.ndjson", the legacy bare "quarantine.ndjson", and the
    # one-off "resistors_quarantine_zero_r.ndjson". Matching only the modern form reported
    # 12,163 withdrawn records as coverage holes and buried the real one.
    r".*quarantine.*\.ndjson$": "quarantined records are withdrawn by definition; they "
                                "carry a quarantineReason and are excluded from queries",
    r".*\.bak$|.*\.backup\.ndjson$": "historical artifact, deliberately frozen",
    r"^converters\.ndjson$": "whole TAS documents, validated against TAS.json by "
                             "tests/test_data.py rather than per-record by family",
    r".*\.pending_series_expanded\.ndjson$": "staging area for the librarian's series "
                                             "expansion, not yet promoted into the catalogue",
    r"^magnetics_staged_.*\.ndjson$": "single hand-staged record awaiting promotion",
    r"^unprocessed_references\.ndjson$": "a work queue of references, not part records",
}


def gate_tables(tas: Path):
    """Read the gate's own tables rather than duplicating them here — a second copy would
    drift, and drift is the failure this file exists to catch."""
    src = (tas / "scripts" / "changed_records_gate.py").read_text()
    families = set(re.findall(r'^\s*"([a-z_0-9]+\.ndjson)"\s*:', src, re.M))
    singles = set(re.findall(r'^\s*CIRCUITS\s*=\s*"([^"]+)"', src, re.M))
    return families, singles


def audit(tas: Path):
    if not (tas / "scripts" / "changed_records_gate.py").is_file():
        return None, "changed_records_gate.py not found — cannot audit coverage"
    families, singles = gate_tables(tas)
    gated = families | singles

    present = sorted(p.name for p in (tas / "data").glob("*.ndjson"))
    covered, exempt, holes = [], [], []
    for name in present:
        if name in gated:
            covered.append(name)
            continue
        why = next((r for pat, r in EXEMPT.items() if re.match(pat, name)), None)
        (exempt if why else holes).append((name, why) if why else name)

    stale = sorted(g for g in gated if not (tas / "data" / g).is_file())
    return {
        "gated": sorted(gated),
        "covered": covered,
        "exempt": [n for n, _ in exempt],
        "holes": holes,
        "stale_mappings": stale,
        "records_unexamined": {h: sum(1 for _ in (tas / "data" / h).open(
            encoding="utf-8", errors="replace")) for h in holes},
    }, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=str(TAS))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    res, err = audit(Path(a.repo))
    if res is None:
        print(f"gate-coverage CANNOT RUN: {err}", file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps(res, indent=1))
        return 1 if (res["holes"] or res["stale_mappings"]) else 0

    print(f"gate coverage over {a.repo}/data")
    print(f"  {len(res['covered'])} file(s) gated per-record")
    print(f"  {len(res['exempt'])} exempt (declared, with a reason)")
    if res["stale_mappings"]:
        print(f"  STALE: {len(res['stale_mappings'])} mapping(s) for files that no longer "
              f"exist: {', '.join(res['stale_mappings'])}")
    if res["holes"]:
        total = sum(res["records_unexamined"].values())
        print(f"\n  COVERAGE HOLES — {len(res['holes'])} file(s), {total} records that no "
              f"per-record gate examines:")
        for h in res["holes"]:
            print(f"    {h}  ({res['records_unexamined'][h]} records)")
        print("\n  These do not fail anything. They are simply unchecked, which is why "
              "nobody notices. Give each one a gate mapping or an explicit EXEMPT entry "
              "stating why it needs none.")
        return 1
    print("\n  no holes: every data file is gated or explicitly exempt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
