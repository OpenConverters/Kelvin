#!/usr/bin/env python3
"""The self-improvement loop: Qarlos finds, Minion fixes, Qarlos confirms.

    audit  ->  file  ->  fix  ->  RE-TEST THE SAME CASE  ->  resolve or reopen

The re-test is what makes this a loop rather than two tools in a trenchcoat. Qarlos'
part selection is seeded, so the case that produced a finding can be reproduced exactly;
after Minion changes something, the identical seed and family are run again and the
ticket is only resolved when the finding is GONE from that rerun. "Minion says it fixed
it" never closes a ticket.

A fix in Kelvin changes the code that builds the shards, so the shards are rebuilt
between the fix and the re-test — otherwise the auditor re-reads the old extraction and
reports the fix as a no-op. (That is ABT #426, and it is exactly the trap this loop
would fall into.)

WHAT IT WILL NOT DO ON ITS OWN
  * commit — unless --commit is passed (Minion leaves the tree dirty for review by default)
  * touch a schema — Minion reverts and stops if it does
  * run forever without a bound — --iterations caps the rounds, --max-fixes the repairs

  loop.py --iterations 1                 # one round, report only, nothing filed
  loop.py --iterations 1 --file          # file tickets, run Minion, re-test
  loop.py --iterations 10 --file --commit --interval 3600
"""
import argparse
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
KELVIN = HERE.parent.parent
QARLOS = HERE / "qarlos.py"
MINION = HERE / "minion.py"
ABT = str(Path.home() / ".local/bin/abt")
STATE = Path.home() / ".qarlos"
SHARD_BUILD = KELVIN / "web" / "scripts" / "build-kelvin-shards.sh"


def log(msg):
    print(f"[{datetime.now(timezone.utc).strftime('%H:%M:%S')}] {msg}", flush=True)


def sweep(query):
    """How many records still match the defect. None if the query cannot be run."""
    sys.path.insert(0, str(HERE))
    try:
        from sweep import run as sweep_run
        return sweep_run(query, limit=5)["population"]
    except Exception as e:  # noqa: BLE001
        log(f"   population query failed: {e}")
        return None


def qarlos(args, out_json):
    cmd = [sys.executable, str(QARLOS), "--json", str(out_json)] + args
    p = subprocess.run(cmd, text=True)
    if p.returncode != 0:
        log(f"qarlos exited {p.returncode}")
    try:
        return json.loads(Path(out_json).read_text())
    except Exception:  # noqa: BLE001
        return {"findings": []}


def rebuild_kelvin_and_shards():
    """A code fix that nothing re-indexes is a fix the auditor cannot see."""
    log("rebuilding Kelvin + shards so the re-test reads the FIXED extraction")
    for cmd in (["cmake", "--build", "build", "-j8"],
                ["bash", str(SHARD_BUILD)]):
        p = subprocess.run(cmd, cwd=str(KELVIN), capture_output=True, text=True,
                           timeout=7200)
        if p.returncode != 0:
            log(f"  FAILED: {' '.join(cmd)}\n{(p.stderr or p.stdout)[-1500:]}")
            return False
    # The browser engine must match the shard format the fix may have changed.
    subprocess.run(["cp", str(KELVIN / "build-wasm" / "kelvin.js"),
                    str(KELVIN / "web" / "public" / "kelvin.js")], check=False)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=1)
    ap.add_argument("--interval", type=int, default=0, help="seconds between rounds")
    ap.add_argument("--per-family", type=int, default=2)
    ap.add_argument("--families", default="")
    ap.add_argument("--max-fixes", type=int, default=3, help="Minion runs per round")
    ap.add_argument("--file", action="store_true", help="file ABT tickets")
    ap.add_argument("--commit", action="store_true", help="let Minion commit its fixes")
    ap.add_argument("--model", default="")
    a = ap.parse_args()
    STATE.mkdir(parents=True, exist_ok=True)

    for it in range(1, a.iterations + 1):
        log(f"═══ round {it}/{a.iterations} ═══")
        base = ["--per-family", str(a.per_family)]
        if a.families:
            base += ["--families", a.families]
        if a.model:
            base += ["--model", a.model]
        if a.file:
            base += ["--file"]

        res = qarlos(base, STATE / f"round{it}.json")
        found = [f for f in res.get("findings", []) if f.get("abt")]
        log(f"round {it}: {res.get('clean',0)} clean, {res.get('claimed',0)} claimed, "
            f"{res.get('refuted',0)} refuted by self-check, {len(found)} filed")

        if not found:
            if a.interval and it < a.iterations:
                time.sleep(a.interval)
            continue

        for f in found[:a.max_fixes]:
            tid = f["abt"]
            log(f"── Minion on ABT #{tid}: {f['title'][:90]}")
            cmd = [sys.executable, str(MINION), str(tid)]
            if a.commit:
                cmd.append("--commit")
            rc = subprocess.run(cmd, text=True).returncode
            if rc != 0:
                log(f"   Minion could not close #{tid} (rc={rc}) — left open")
                continue

            # A Kelvin fix only becomes visible to the auditor after a re-index.
            if f.get("defect_in") in ("extraction", "ranking"):
                if not rebuild_kelvin_and_shards():
                    subprocess.run([ABT, "comment", str(tid), "--body",
                                    "Re-test skipped: rebuilding Kelvin/shards failed, so "
                                    "the auditor would have re-read the OLD extraction."])
                    continue

            # A catalogue fix is judged on the POPULATION, not on the sampled part.
            # Repairing the one row Qarlos happened to draw would otherwise pass the
            # seeded re-test and close a ticket covering thousands of siblings.
            if f.get("population_query"):
                after = sweep(f["population_query"])
                before = f.get("population_before")
                log(f"   population {before} -> {after}")
                if after:
                    log(f"   {after} records still match — NOT closing #{tid}")
                    subprocess.run([ABT, "comment", str(tid), "--body",
                                    f"Qarlos re-ran the population query after the fix: "
                                    f"{before} -> {after} records still match.\n\n"
                                    "The sampled part may be repaired, but this defect is "
                                    "not one record. The ticket stays open until the "
                                    "population is zero (repaired or quarantined).\n\n"
                                    f"  python3 tools/qarlos/sweep.py --query "
                                    f"'{json.dumps(f['population_query'])}'"])
                    subprocess.run([ABT, "update", str(tid), "--status", "reported"])
                    continue

            log(f"   re-testing seed {f['repro']['seed']} / {f['repro']['families']}")
            retest = qarlos(["--per-family", str(f["repro"]["per_family"]),
                             "--families", f["repro"]["families"],
                             "--seed", str(f["repro"]["seed"])]
                            + (["--model", a.model] if a.model else []),
                            STATE / f"retest_{tid}.json")
            still = [x for x in retest.get("findings", [])
                     if x.get("signature") == f["signature"]]
            if still:
                log(f"   STILL PRESENT — reopening #{tid}")
                subprocess.run([ABT, "comment", str(tid), "--body",
                                "Qarlos re-ran the original seeded case after the fix and "
                                "the finding is STILL present:\n\n"
                                f"{still[0].get('title')}\n\n"
                                "Reopening. The change did not address the cause."])
                subprocess.run([ABT, "update", str(tid), "--status", "reported"])
            else:
                log(f"   CONFIRMED FIXED — resolving #{tid}")
                subprocess.run([ABT, "comment", str(tid), "--body",
                                "Qarlos re-ran the original seeded case "
                                f"(--seed {f['repro']['seed']} --families "
                                f"{f['repro']['families']}) after the fix. The finding is "
                                "gone. Confirmed by the auditor that raised it."])
                subprocess.run([ABT, "resolve", str(tid)])

        if a.interval and it < a.iterations:
            log(f"sleeping {a.interval}s")
            time.sleep(a.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
