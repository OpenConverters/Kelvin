#!/usr/bin/env python3
"""Calibrate Qarlos' self-check: does the verifier actually refute anything?

A verifier that confirms everything is not a verifier, and its output would read exactly
like a working one — every finding "confirmed", nothing refuted. So it gets fed claims
that are KNOWN false and must reject them, alongside a claim that is known true and must
survive. Run this after touching either prompt.

  selftest.py [--families connector]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from qarlos import compact, run_probe, verify  # noqa: E402

def _oversize(case):
    """A claim that a LARGER capacitor passing is a defect — using the case's own numbers.

    The only honest way to refute this is the design rule (oversizing bulk capacitance is
    normal, and the accept window is 0.80-4.00x). If the claim quoted invented values, or
    named a candidate that is actually smaller, the verifier would refute it on the
    arithmetic and the rule under test would never be exercised — the calibration would
    pass while knowing nothing. Returns None when this case has no larger candidate, so
    the check SKIPS loudly instead of passing vacuously.
    """
    spec = case.get("origSpec") or {}
    orig = spec.get("value_si")
    if not isinstance(orig, (int, float)) or orig <= 0:
        return None
    # Only a BULK-duty original makes oversizing legitimate. A picofarad class-1 ceramic
    # is an RF/resonant element whose value IS the function, and a verifier that CONFIRMS
    # an oversize complaint there is right, not rubber-stamping — an earlier version of
    # this check used a 2.1 pF C0G part and failed the calibration for that reason.
    if orig < 1e-6 or str(spec.get("technology", "")).startswith("ceramic-class-1"):
        return None
    for c in case.get("ranked", []):
        cap = ((c.get("_row") or {}).get("capacitance"))
        if isinstance(cap, (int, float)) and orig < cap <= 4 * orig:
            return {
                "severity": "medium", "defect_in": "ranking",
                "title": "A larger capacitor is graded 'pass' against a smaller original",
                "candidate": c.get("mpn"),
                "what_is_wrong": f"{c.get('mpn')} is graded a passing substitute.",
                "evidence": (f"The original is {orig:.4g} F and {c.get('mpn')} is "
                             f"{cap:.4g} F — {100 * (cap / orig - 1):.0f}% larger, well "
                             f"outside the original's declared tolerance band."),
                "expected": "It should have failed the value comparison.",
            }
    return None


# Claims that must be REFUTED, each wrong in a different way the verifier is told to check.
BOGUS = [
    {"why": "quotes a value that is not in the data at all",
     "finding": {"severity": "critical", "defect_in": "catalogue",
                 "title": "Rated voltage is recorded as 999999 V",
                 "candidate": "<first>",
                 "what_is_wrong": "The tool accepted a part rated 999999 V.",
                 "evidence": "The raw record shows ratedVoltage: 999999.",
                 "expected": "It should have rejected the impossible rating."}},
    {"why": "calls correct honest behaviour a defect",
     "finding": {"severity": "high", "defect_in": "ranking",
                 "title": "Tool marks parameters 'unverified' instead of comparing them",
                 "candidate": "<first>",
                 "what_is_wrong": "Several parameters come back 'unverified'.",
                 "evidence": "param_verdicts contains 'unverified' entries.",
                 "expected": "Every parameter should get pass or fail."}},
    # Built from the case's REAL capacitances. Quoting invented numbers would let the
    # verifier refute on "that value is not in the data" (rule 1) without ever exercising
    # the rule under test — it would pass while knowing nothing about oversizing.
    {"why": "calls a capacitance oversize inside the accept window a defect",
     "needs": "capacitor", "build": lambda case, first: _oversize(case)},
    {"why": "claims an extraction mismatch where raw and extracted agree",
     "finding": {"severity": "high", "defect_in": "extraction",
                 "title": "The MPN is extracted wrongly",
                 "candidate": "<first>",
                 "what_is_wrong": "Kelvin's row carries a different part number than the raw record.",
                 "evidence": "The raw record and the extracted row disagree on the MPN.",
                 "expected": "They should match."}},
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default="connector")
    # Seeded, so a calibration failure is reproducible. Some checks only apply to some
    # cases (the oversize rule needs a BULK original), hence the loud SKIP lines: pick a
    # seed whose case can exercise what you changed.
    ap.add_argument("--seed", type=int, default=20260801)
    ap.add_argument("--model", default="")
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()

    class A:
        per_family, max_results = 1, 8
        seed = a.seed
        families, shards = a.families, ""
    cases, _ = run_probe(A)
    case = next((c for c in cases if c.get("ranked")), None)
    if not case:
        sys.exit("no case with candidates to calibrate against")
    first = case["ranked"][0].get("mpn")
    print(f"calibrating against {case['family']} / {case['original'].get('mpn')} "
          f"vs {first}\n")

    bad = skipped = 0
    for b in BOGUS:
        if b.get("needs") and b["needs"] != case["family"]:
            print(f"  [skip] needs a {b['needs']} case — {b['why']}")
            continue
        f = b["build"](case, first) if b.get("build") else dict(b["finding"])
        if f is None:
            print(f"  [SKIP] this case cannot exercise it — {b['why']}")
            skipped += 1
            continue
        f["candidate"] = first if f.get("candidate") == "<first>" else f.get("candidate")
        v = verify(case, f, a.model, a.timeout)
        ok = v.get("verdict") == "refuted"
        print(f"  [{'PASS' if ok else 'FAIL'}] must refute — {b['why']}")
        print(f"         verdict={v.get('verdict')}: {v.get('reason','')[:180]}")
        if not ok:
            bad += 1

    print()
    if bad:
        print(f"{bad}/{len(BOGUS)} bogus claims were CONFIRMED — the self-check is "
              f"rubber-stamping and its 'confirmed' verdicts mean nothing. Fix the "
              f"verifier prompt before trusting a run.")
        return 1
    ran = len(BOGUS) - skipped
    print(f"self-check refuted all {ran} bogus claims it could run"
          + (f" ({skipped} skipped — this case could not exercise them)" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
