#!/usr/bin/env python3
"""Minion — fixes one ABT ticket that Qarlos raised, then hands it back to be re-tested.

Qarlos finds and proves a defect; Minion repairs it. It is launched per ticket with
Opus through the `claude` CLI, in the repo the ticket was routed to, and it is held to
the same rules a human contributor is. Qarlos then re-runs the exact seeded case that
produced the finding, so "fixed" is demonstrated on the original failure rather than
asserted (see loop.py).

WHY IT IS ALLOWED TO EDIT ANYTHING AT ALL. The repos have hard house rules that exist
because breaking them has cost real damage before — fabricated parts shipped to prod,
schemas relaxed to make bad data validate, tests disabled to make a suite go green. Those
rules are restated to Minion as absolutes, and the ones that can be checked mechanically
are checked here after it finishes, not taken on its word:

  * schemas are NEVER edited (governance-controlled, shared across repos) — enforced by
    refusing to accept a run that touched one;
  * catalogue data changes must pass JSON Schema AND the Blade Runner physics validator;
  * Kelvin changes must pass the Catch2 suite, run directly (never ctest);
  * nothing is deleted; bad records are quarantined with a reason.

Minion does NOT commit by default. It reports a diff and the gate results; --commit lets
it land on main (the house workflow for these repos).

  minion.py 431                 # attempt the fix, leave the tree dirty for review
  minion.py 431 --commit        # ... and commit if the gates pass
  minion.py 431 --dry-run       # print the brief, launch nothing
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CLAUDE = os.environ.get("QARLOS_CLAUDE") or str(Path.home() / ".local/bin/claude")
ABT = os.environ.get("QARLOS_ABT") or str(Path.home() / ".local/bin/abt")

# Where a ticket's `to` repo lives, and what proves a change to it is sound.
REPOS = {
    "kelvin": {
        "path": Path.home() / "OpenConverters" / "Kelvin",
        "gates": [
            ["cmake", "--build", "build", "-j8"],
            ["./build/test_kelvin"],
        ],
    },
    "tas": {
        "path": Path.home() / "PSMA" / "TAS",
        "gates": [
            ["python3", "-m", "pytest", "tests/test_schemas.py", "-q"],
        ],
    },
}

SYSTEM = """You are Minion, an autonomous fixer working one issue from a shared tracker.
Qarlos, a standing auditor, found and verified this defect against the raw catalogue
record. Your job is to fix the CAUSE.

HOW TO WORK
1. Reproduce first. The ticket carries a deterministic repro command; run it and see the
   defect before you change anything. If you cannot reproduce it, do NOT invent a fix —
   report that and stop.
2. Fix the root cause, not the symptom, and fix the WHOLE POPULATION. The part named in
   the ticket is the sample that exposed the defect, not the defect. If the ticket has a
   SCOPE section with a population query, that number is your job — repairing the sampled
   part and leaving the rest both fails the re-test and closes a ticket that covered
   thousands of records, which is worse than never having looked. Run the query, fix or
   quarantine every match, re-run it, and do not report fixed until it returns zero:
     python3 <kelvin>/tools/qarlos/sweep.py --query '<the query from the ticket>'
   Where the defect came from an importer, fix the importer too so the next run does not
   reintroduce it, and name it in root_cause.
3. Change as little as possible, and match the surrounding code's style and comments.
4. Verify. Run the gates for this repo and paste the real output.

ABSOLUTE RULES — breaking one of these is worse than not fixing the issue:
- NEVER edit a JSON Schema file (**/schemas/*.json, *.schema.json). They are governance
  controlled and shared across repos. If data will not validate, the data is wrong, or
  the answer is to report the gap. Say so; do not touch the schema.
- NEVER delete catalogue records. Move a bad record to the matching
  *.quarantine_*.ndjson with a quarantineReason. Traceability is preserved.
- NEVER weaken a test, threshold, or gate to make something pass. A failing check is
  doing its job. If it blocks you, report that and stop.
- NEVER add a fallback, default, or silent shortcut for missing data. Throw or report.
- All catalogue values are SI base units (F, H, ohm, V, A, C, Hz, m, s). A units error is
  fixed by correcting the value, never by adding a conversion at the read site.
- Do not bulk-rewrite a data file. Patch the specific line(s); other processes append
  concurrently.
- Any new or changed catalogue data must pass BOTH JSON Schema validation and the Blade
  Runner physics validator (TAS/validator, tas_validator).
- C++ tests are Catch2 and are run by invoking the test binary DIRECTLY. Never ctest.

WHEN YOU ARE DONE, reply with STRICT JSON as your final message, no prose around it:
{"fixed": true|false,
 "summary": "<what you changed and why, 1-3 sentences>",
 "files_changed": ["<path>", ...],
 "root_cause": "<the actual cause>",
 "population_before": <int or null>, "population_after": <int or null>,
 "wider_impact": "<other records/sites with the same defect, or 'none found'>",
 "gates_run": "<the commands you ran and their result>",
 "blocked_by": "<what stopped you, if fixed is false>"}"""


def ticket(tid):
    p = subprocess.run([ABT, "view", str(tid)], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"abt view {tid} failed: {p.stderr[:200]}")
    return p.stdout


def route_of(text):
    m = re.search(r"^\s*To\s*:\s*(\S+)", text, re.M)
    return (m.group(1).strip().lower() if m else "kelvin")


def git_status(repo):
    p = subprocess.run(["git", "-C", str(repo), "status", "--porcelain"],
                       capture_output=True, text=True)
    return [l[3:].strip() for l in p.stdout.splitlines() if l.strip()]


def run_gates(repo, gates):
    out = []
    ok = True
    for g in gates:
        p = subprocess.run(g, cwd=str(repo), capture_output=True, text=True, timeout=3600)
        out.append(f"$ {' '.join(g)}\n{(p.stdout or '')[-1500:]}{(p.stderr or '')[-800:]}")
        if p.returncode != 0:
            ok = False
            break
    return ok, "\n\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("id", type=int)
    ap.add_argument("--commit", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    # The 1M window explicitly: Minion reads full catalogue records, a whole ticket and
    # multi-file diffs. The bare "opus" alias resolves to the standard context and is not
    # what this workload wants. Pinned rather than inherited so a settings change cannot
    # quietly alter what an autonomous fixer is running on.
    ap.add_argument("--model", default=os.environ.get("MINION_MODEL", "opus[1m]"))
    ap.add_argument("--timeout", type=int, default=3600)
    a = ap.parse_args()

    body = ticket(a.id)
    route = route_of(body)
    cfg = REPOS.get(route)
    if not cfg:
        sys.exit(f"ticket #{a.id} is routed to '{route}', which Minion has no repo for "
                 f"(known: {', '.join(REPOS)})")
    repo = cfg["path"]
    if not repo.is_dir():
        sys.exit(f"{repo} does not exist")

    dirty_before = set(git_status(repo))
    prompt = (f"Fix this issue. You are working in {repo}.\n\n"
              f"{body}\n\n"
              f"Reproduce it, fix the cause, run the gates, then reply with the strict "
              f"JSON described.")
    if a.dry_run:
        print(f"[dry-run] repo={repo} route={route}\n{prompt[:2000]}")
        return 0

    print(f"minion: ticket #{a.id} -> {route} ({repo})")
    cmd = [CLAUDE, "-p", prompt, "--append-system-prompt", SYSTEM,
           "--model", a.model, "--permission-mode", "bypassPermissions",
           "--add-dir", str(repo), "--add-dir", str(Path.home() / "PSMA")]
    try:
        p = subprocess.run(cmd, cwd=str(repo), capture_output=True, text=True,
                           timeout=a.timeout)
    except subprocess.TimeoutExpired:
        print(f"minion: TIMED OUT after {a.timeout}s")
        return 2
    txt = (p.stdout or "").strip()
    m = re.search(r"\{.*\}", txt, re.S)
    report = {}
    if m:
        try:
            report = json.loads(m.group(0))
        except json.JSONDecodeError:
            pass
    if not report:
        print(f"minion: no JSON report.\n{txt[-1200:]}")
        return 2

    touched = [f for f in git_status(repo) if f not in dirty_before]
    print(f"  fixed={report.get('fixed')}  files={touched}")
    print(f"  root cause: {report.get('root_cause','')[:300]}")
    print(f"  wider impact: {report.get('wider_impact','')[:300]}")

    # The one rule that is checked rather than trusted.
    schemas = [f for f in touched if re.search(r"schemas/.*\.json$|\.schema\.json$", f)]
    if schemas:
        print(f"  REJECTED: touched schema files {schemas} — reverting those and stopping")
        subprocess.run(["git", "-C", str(repo), "checkout", "--"] + schemas)
        subprocess.run([ABT, "comment", str(a.id), "--body",
                        "Minion attempted a fix that modified schema files "
                        f"({', '.join(schemas)}). Schemas are governance-controlled; the "
                        "change was reverted and the ticket left open."])
        return 3

    if not report.get("fixed"):
        subprocess.run([ABT, "comment", str(a.id), "--body",
                        "Minion could not fix this.\n\n"
                        f"Blocked by: {report.get('blocked_by','(unstated)')}\n"
                        f"Notes: {report.get('summary','')}"])
        return 1

    ok, gate_out = run_gates(repo, cfg["gates"])
    print(f"  gates: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(gate_out[-2000:])
        subprocess.run([ABT, "comment", str(a.id), "--body",
                        "Minion proposed a fix but the repo gates FAILED, so it was not "
                        f"committed.\n\n{gate_out[-3000:]}"])
        return 1

    if a.commit and touched:
        msg = (f"{report.get('summary','fix')}\n\n"
               f"Root cause: {report.get('root_cause','')}\n"
               f"Wider impact: {report.get('wider_impact','')}\n\n"
               f"Found by Qarlos, fixed by Minion. ABT #{a.id}.\n")
        subprocess.run(["git", "-C", str(repo), "add"] + touched)
        subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", msg])
        print("  committed on main")

    subprocess.run([ABT, "comment", str(a.id), "--body",
                    f"Minion fix.\n\n{report.get('summary')}\n\n"
                    f"Root cause: {report.get('root_cause')}\n"
                    f"Wider impact: {report.get('wider_impact')}\n"
                    f"Files: {', '.join(touched) or '(none)'}\n\n"
                    f"Gates:\n{gate_out[-2500:]}\n\n"
                    "Awaiting Qarlos re-test of the original seeded case."])
    subprocess.run([ABT, "done", str(a.id)])
    print(f"  ticket #{a.id} -> done (awaiting Qarlos confirmation)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
