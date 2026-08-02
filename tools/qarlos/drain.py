#!/usr/bin/env python3
"""Drain the Minion backlog: every reported ticket the auditor filed gets its fix.

loop.py only runs Minion on the findings ITS round just filed; anything beyond
--max-fixes, and anything filed by a manual or report-only run, stays 'reported'
on the board forever. This walks the board instead. Same closing discipline as
the loop: a ticket is resolved only when the auditor's own evidence is gone —
the population query returns zero AND the seeded re-test no longer claims the
candidate. "Minion says it fixed it" never closes a ticket.

The round state JSONs are overwritten hourly, so the durable record is the
ticket body itself: the REPRODUCE line pins the seed/families, the sweep query
pins the population. Both are parsed from `abt view`. A ticket whose body has
neither is left for a human, loudly.

  drain.py                      # list what would run, touch nothing
  drain.py --go                 # run Minion WITHOUT commit (tree left for review)
  drain.py --go --commit        # the cron mode
  drain.py --go --commit --max 6 --only 485,504 --include 494

Serialize with the hourly loop via the same lock the cron uses:
  flock /home/alf/.qarlos/loop.lock python3 tools/qarlos/drain.py --go --commit
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from loop import ABT, MINION, STATE, log, qarlos, rebuild_kelvin_and_shards, sweep  # noqa: E402

PRIORITY = {"critical": 0, "high": 1, "medium": 2, "low": 3}


def board():
    """Reported tickets filed by the auditor: [(id, priority, to, title)]."""
    p = subprocess.run([ABT, "list"], capture_output=True, text=True)
    rows = []
    for line in p.stdout.splitlines():
        m = re.match(r"#\s*(\d+)\s+(\w+)\s+(\w+)\s+(\S+)\s+(\S+)\s+(.*)", line)
        if m and m.group(2) == "reported" and m.group(4) == "qarlos":
            rows.append((int(m.group(1)), m.group(3), m.group(5), m.group(6)))
    return rows


def ticket_body(tid):
    return subprocess.run([ABT, "view", str(tid)], capture_output=True, text=True).stdout


def parse_repro(body):
    """seed/families/per-family from the REPRODUCE line (qarlos.py or probe.mjs form)."""
    seed = re.search(r"--seed\s+(\d+)", body)
    fam = re.search(r"--families\s+([\w,]+)", body)
    per = re.search(r"--per-family\s+(\d+)", body)
    if not (seed and fam):
        return None
    return {"seed": int(seed.group(1)), "families": fam.group(1),
            "per_family": int(per.group(1)) if per else 1}


def parse_population_query(body):
    """The sweep query the auditor filed as the defect's population, if any.

    abt view wraps long bodies, so the JSON may span lines with leading
    indentation; join and strip before parsing. A query that will not parse is
    treated as absent — the seeded re-test still gates the close.
    """
    m = re.search(r"--query\s+'(.*?)'", body, re.DOTALL)
    if not m:
        return None
    text = "".join(ln.strip() for ln in m.group(1).splitlines())
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        log("   population query in ticket body did not parse — skipping the sweep gate")
        return None


def still_claimed(retest, body):
    """Findings from the seeded re-test that involve a candidate the ticket names.

    Conservative on the open side: ANY surviving claim on a candidate mentioned
    in the ticket keeps it open, even if the wording changed. A claim on an
    unrelated candidate does not (it is a new finding for a filing round, not
    evidence this ticket's defect survived).
    """
    hits = []
    for f in retest.get("findings", []):
        if f.get("refuted"):
            continue
        cand = f.get("candidate")
        if cand and cand in body:
            hits.append(f)
    return hits


def drain_one(tid, to, title, do_commit):
    body = ticket_body(tid)
    repro = parse_repro(body)
    popq = parse_population_query(body)
    log(f"── Minion on ABT #{tid} [{to}]: {title[:90]}")

    cmd = [sys.executable, str(MINION), str(tid)]
    if do_commit:
        cmd.append("--commit")
    rc = subprocess.run(cmd, text=True).returncode
    if rc == 4:
        # Minion committed and then found that a CLEAN checkout of HEAD fails its own
        # gates — the working tree was hiding a missing half (lessons: verify-from-clean-
        # head). Draining further would pile commits onto a broken main, and every
        # subsequent Minion would inherit a red baseline and be unable to tell its own
        # breakage from the inherited one.
        log(f"   Minion left main RED after #{tid} — halting the drain")
        return "head_broken"
    if rc == 5:
        # Quota or transport, not a verdict on the ticket. Continuing would burn the rest
        # of the queue writing false "could not fix" notes — which is exactly what the
        # 2026-08-02 nightly did to 11 tickets after the weekly limit hit at 15:28.
        log(f"   Minion CANNOT RUN (infrastructure/quota) — halting, #{tid} untouched")
        return "cannot_run"
    if rc != 0:
        log(f"   Minion could not close #{tid} (rc={rc}) — left open")
        return "minion_failed"

    # A Kelvin fix only becomes visible to the auditor after a re-index.
    if to == "kelvin":
        if not rebuild_kelvin_and_shards():
            subprocess.run([ABT, "comment", str(tid), "--body",
                            "Re-test skipped: rebuilding Kelvin/shards failed, so the "
                            "auditor would have re-read the OLD extraction."])
            return "rebuild_failed"

    # A catalogue fix is judged on the POPULATION, not on the sampled part.
    if popq:
        after = sweep(popq)
        before = re.search(r"(\d[\d,]*)\s+of\s+[\d,]+\s+in\s", body)
        log(f"   population {before.group(1) if before else '?'} -> {after}")
        if after:
            subprocess.run([ABT, "comment", str(tid), "--body",
                            f"Backlog drain re-ran the population query after the fix: "
                            f"{after} records still match. The ticket stays open until "
                            f"the population is zero (repaired or quarantined).\n\n"
                            f"  python3 tools/qarlos/sweep.py --query '{json.dumps(popq)}'"])
            subprocess.run([ABT, "update", str(tid), "--status", "reported"])
            return "population_survives"

    if not repro:
        subprocess.run([ABT, "comment", str(tid), "--body",
                        "Backlog drain: Minion reports fixed"
                        + (" and the population query returns zero" if popq else "")
                        + ", but the ticket body carries no parseable REPRODUCE seed, so "
                        "the seeded re-test cannot run. Left at 'fixed' for a manual "
                        "confirmation — not resolved without the auditor's evidence."])
        return "no_seed"

    log(f"   re-testing seed {repro['seed']} / {repro['families']}")
    retest = qarlos(["--per-family", str(repro["per_family"]),
                     "--families", repro["families"], "--seed", str(repro["seed"])],
                    STATE / f"drain_retest_{tid}.json")
    hits = still_claimed(retest, body)
    if hits:
        log(f"   STILL PRESENT — reopening #{tid}")
        subprocess.run([ABT, "comment", str(tid), "--body",
                        "Backlog drain re-ran the original seeded case after the fix and "
                        "a claim on this ticket's candidate is STILL present:\n\n"
                        + "\n".join(f"  - {h.get('title')}" for h in hits[:3])
                        + "\n\nReopening. The change did not address the cause."])
        subprocess.run([ABT, "update", str(tid), "--status", "reported"])
        return "still_present"

    log(f"   CONFIRMED — resolving #{tid}")
    subprocess.run([ABT, "comment", str(tid), "--body",
                    f"Backlog drain: Qarlos re-ran the original seeded case (--seed "
                    f"{repro['seed']} --families {repro['families']}) after the fix"
                    + (", and the population query returns zero" if popq else "")
                    + ". The finding is gone. Confirmed by the auditor's own evidence."])
    subprocess.run([ABT, "resolve", str(tid)])
    return "resolved"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true", help="actually run (default: list only)")
    ap.add_argument("--commit", action="store_true", help="let Minion commit its fixes")
    ap.add_argument("--max", type=int, default=0, help="cap tickets this run (0 = all)")
    ap.add_argument("--only", default="", help="comma-separated ticket ids to restrict to")
    ap.add_argument("--include", default="", help="extra ticket ids to add (e.g. 494)")
    a = ap.parse_args()
    STATE.mkdir(parents=True, exist_ok=True)

    rows = board()
    if a.include:
        have = {t for t, *_ in rows}
        for tid in (int(x) for x in a.include.split(",") if x.strip()):
            if tid in have:
                continue
            p = subprocess.run([ABT, "list"], capture_output=True, text=True)
            m = re.search(rf"#\s*{tid}\s+reported\s+(\w+)\s+\S+\s+(\S+)\s+(.*)", p.stdout)
            if m:
                rows.append((tid, m.group(1), m.group(2), m.group(3)))
            else:
                log(f"--include {tid}: not a reported ticket on the board — skipped")
    if a.only:
        keep = {int(x) for x in a.only.split(",") if x.strip()}
        rows = [r for r in rows if r[0] in keep]
    rows.sort(key=lambda r: (PRIORITY.get(r[1], 9), r[0]))
    if a.max:
        rows = rows[:a.max]

    log(f"backlog: {len(rows)} ticket(s)"
        + ("" if a.go else " — dry list, pass --go to run"))
    for tid, pri, to, title in rows:
        log(f"  #{tid} [{pri}/{to}] {title[:100]}")
    if not a.go:
        return 0

    tally = {}
    for tid, pri, to, title in rows:
        outcome = drain_one(tid, to, title, a.commit)
        tally[outcome] = tally.get(outcome, 0) + 1
        if outcome == "cannot_run":
            log("HALTED: the model is unavailable. Nothing is wrong with the queue; "
                "re-run when capacity returns.")
            break
        if outcome == "head_broken":
            # Everything after this would build on a red baseline and could not tell its
            # own breakage from the inherited one.
            log("HALTED: main does not pass its own gates from a clean checkout. "
                "Repair it before draining further.")
            break
    log("drain complete: " + " | ".join(f"{k} {v}" for k, v in sorted(tally.items())))
    if tally.get("cannot_run"):
        return 3
    return 2 if tally.get("head_broken") else 0


if __name__ == "__main__":
    sys.exit(main())
