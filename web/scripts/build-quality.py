#!/usr/bin/env python3
"""Build quality.json for the public /quality page and /api/v1/quality (ABT #1924).

    python3 web/scripts/build-quality.py [OUT]            # default web/.quality/quality.json
    TAS=/path/to/TAS                                      # default /home/alf/PSMA/TAS

Reads the LAST line of TAS docs/metrics/history.ndjson (written by scripts/db_metrics.py) and
republishes it unchanged for the twelve families Kelvin serves — no number is recomputed or
rounded here, so the page cannot say anything the metrics run did not. Refuses a run older than
MAX_AGE_DAYS: a quality page that quietly shows last month's numbers is worse than none.
"""
import datetime
import json
import os
import sys

TAS = os.environ.get("TAS", "/home/alf/PSMA/TAS")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".quality", "quality.json")
MAX_AGE_DAYS = int(os.environ.get("MAX_AGE_DAYS", "14"))
# Kelvin family -> TAS catalogue name in the metrics run.
FAMILIES = {"mosfet": "mosfets", "diode": "diodes", "capacitor": "capacitors",
            "resistor": "resistors", "controller": "controllers", "igbt": "igbts", "bjt": "bjts",
            "varistor": "varistors", "magnetic": "magnetics", "analog": "analog_ics",
            "timing": "timing_devices", "connector": "connectors"}
CHECKS = ("existence", "traced", "fields", "curves", "url", "valid")


def main() -> int:
    path = os.path.join(TAS, "docs", "metrics", "history.ndjson")
    with open(path) as fh:
        lines = [ln for ln in fh if ln.strip()]
    run = json.loads(lines[-1])
    age = (datetime.date.today() - datetime.date.fromisoformat(run["date"])).days
    if age > MAX_AGE_DAYS:
        sys.exit(f"REFUSING: the newest metrics run is {run['date']} ({age} days old, limit "
                 f"{MAX_AGE_DAYS}); run scripts/db_metrics.py first")
    families = {}
    for family, catalogue in FAMILIES.items():
        c = run["by_catalogue"].get(catalogue)
        if c is None:
            sys.exit(f"REFUSING: catalogue {catalogue!r} missing from the {run['date']} metrics run")
        families[family] = {
            "live": c["live"], "quarantined": c["quarantine"], "quality": c["quality"],
            "coverage": c["coverage"],
            "checks": {k: c["checks"].get(k) for k in CHECKS}}   # None = not applicable (no curves)
    doc = {"date": run["date"], "specVersion": run["spec_version"],
           "wholeDatabase": {"quality": run["quality"], "coverage": run["coverage"]},
           "families": families}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as fh:
        json.dump(doc, fh, indent=1)
    print(f"quality.json from the {run['date']} run -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
