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
    ap.add_argument("--model", default="")
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()

    class A:
        per_family, max_results, seed = 1, 6, 20260801
        families, shards = a.families, ""
    cases, _ = run_probe(A)
    case = next((c for c in cases if c.get("ranked")), None)
    if not case:
        sys.exit("no case with candidates to calibrate against")
    first = case["ranked"][0].get("mpn")
    print(f"calibrating against {case['family']} / {case['original'].get('mpn')} "
          f"vs {first}\n")

    bad = 0
    for b in BOGUS:
        f = dict(b["finding"])
        f["candidate"] = first if f["candidate"] == "<first>" else f["candidate"]
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
    print(f"self-check refuted all {len(BOGUS)} bogus claims.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
