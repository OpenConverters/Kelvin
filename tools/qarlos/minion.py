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
sys.path.insert(0, str(HERE))
import guards      # noqa: E402  — the lessons that are checked, not told
import lessons     # noqa: E402  — the lessons that are told
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
        # Run against a detached worktree at HEAD after committing, so a suite that only
        # passes because of somebody's uncommitted work cannot certify the commit. Only
        # when the change touched CODE — data cannot break a build, and a full configure
        # in a fresh worktree is expensive.
        "clean_head_gates": [
            ["cmake", "-B", "build", "-G", "Ninja"],
            ["cmake", "--build", "build", "-j8"],
            ["./build/test_kelvin"],
        ],
    },
    "tas": {
        "path": Path.home() / "PSMA" / "TAS",
        "gates": [
            ["python3", "-m", "pytest", "tests/test_schemas.py", "-q"],
            # The schema tests never look at DATA — they prove the schemas parse and
            # cross-refer. Without this second gate an autonomous fixer editing a
            # catalogue had nothing standing between it and the data except its own
            # instructions, which is not a gate. This validates every record of any
            # catalogue file the change touched against BOTH the family schema and the
            # Blade Runner physics validator. Slow on purpose (~7 min for capacitors):
            # it cannot use the diff, because data/*.ndjson is git-LFS and `git diff`
            # renders the 3-line pointer rather than the records.
            ["python3", "scripts/changed_records_gate.py"],
        ],
        "clean_head_gates": [
            ["python3", "-m", "pytest", "tests/test_schemas.py", "-q"],
        ],
    },
}

# A change confined to these cannot break a build or a test binary, so clean-HEAD
# verification is skipped for it — it would cost a full configure to prove nothing.
DATA_ONLY = re.compile(r"^(data/|staging/)")

# The model could not be reached or was out of quota. This is NOT a statement about the
# ticket, and must never be written to the tracker as one.
INFRA_FAILURE = re.compile(
    r"hit your (weekly|usage|5-hour) limit|usage limit reached|rate.?limit|"
    r"quota (exceeded|exhausted)|overloaded_error|api_error|"
    r"Connection (error|refused)|credit balance is too low",
    re.I)

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
- Commit with an explicit pathspec. Several agents share this working tree and its index;
  `git add` followed by a later `git commit` will sweep in whatever else is staged.

WHEN YOU ARE DONE, reply with STRICT JSON as your final message, no prose around it:
{"fixed": true|false,
 "summary": "<what you changed and why, 1-3 sentences>",
 "files_changed": ["<path>", ...],
 "root_cause": "<the actual cause>",
 "population_before": <int or null>, "population_after": <int or null>,
 "wider_impact": "<other records/sites with the same defect, or 'none found'>",
 "calibration": "<REQUIRED if you added or tightened a detection rule: how many EXISTING
   records it fires on, over what denominator, per rule. '' if you changed no rules.>",
 "gates_run": "<the commands you ran and their result>",
 "blocked_by": "<what stopped you, if fixed is false>"}

""" + lessons.render("minion")


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
        # A full-catalogue gate can legitimately run for many minutes; a timeout that
        # kills it would read as "gate failed" and block a good fix.
        p = subprocess.run(g, cwd=str(repo), capture_output=True, text=True, timeout=7200)
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
    # Effort is pinned for the same reason the model is: inherited, it comes from
    # ~/.claude/settings.json (xhigh), so an editor preference would silently change
    # what an autonomous fixer runs at. `high` rather than `xhigh` — on Opus 5 the
    # lower levels are strong enough that xhigh buys little on mechanical repair work,
    # and effort is the cost lever that does not touch the model tier (2026-08-02).
    ap.add_argument("--effort", default=os.environ.get("MINION_EFFORT", "high"),
                    choices=["low", "medium", "high", "xhigh", "max"])
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
    head_before = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                 capture_output=True, text=True).stdout.strip()
    prompt = (f"Fix this issue. You are working in {repo}.\n\n"
              f"{body}\n\n"
              f"Reproduce it, fix the cause, run the gates, then reply with the strict "
              f"JSON described.")
    if a.dry_run:
        print(f"[dry-run] repo={repo} route={route}\n{prompt[:2000]}")
        return 0

    print(f"minion: ticket #{a.id} -> {route} ({repo})")
    cmd = [CLAUDE, "-p", prompt, "--append-system-prompt", SYSTEM,
           "--model", a.model, "--effort", a.effort,
           "--permission-mode", "bypassPermissions",
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
        # A fixer that CANNOT RUN is not a fixer that could not fix. On 2026-08-02 the
        # nightly drain exhausted the API quota and then marched through 11 more tickets,
        # writing "Minion could not fix this. Blocked by: (unstated)" on every one — a
        # judgement about the tickets that was really a fact about the account. Those
        # comments are indistinguishable from a real assessment and could get a live
        # defect deprioritised.
        blob = (txt + (p.stderr or ""))[-4000:]
        if INFRA_FAILURE.search(blob):
            why = INFRA_FAILURE.search(blob).group(0)
            print(f"minion: CANNOT RUN ({why}) — ticket untouched, no verdict recorded")
            return 5      # distinct from 2: infrastructure, not a failed fix
        print(f"minion: no JSON report.\n{txt[-1200:]}")
        return 2

    # What this run changed = still-dirty files PLUS anything it committed itself.
    # Minion's agent frequently commits its own work before returning, which left the
    # dirty-set diff empty and reported "Files: (none)" for a run that had just repaired
    # 183 records, fixed two importers and added a validator rule (ABT #524, 2026-08-02).
    # Every guard hangs off this list, so an empty one silently disabled all of them and
    # skipped the pathspec commit and the clean-HEAD check.
    head_now = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
    committed_here = []
    if head_before and head_now and head_before != head_now:
        d = subprocess.run(["git", "-C", str(repo), "diff", "--name-only",
                            f"{head_before}..{head_now}"], capture_output=True, text=True)
        committed_here = [f for f in d.stdout.split() if f]
    touched = sorted(set(f for f in git_status(repo) if f not in dirty_before)
                     | set(committed_here))
    self_committed = bool(committed_here)
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

    # Guards — the lessons that are checked rather than told (see lessons.py/guards.py).
    # These run BEFORE the slow repo gates: a rule change with no calibration, or a
    # producer defect with the producer untouched, should not cost seven minutes of
    # catalogue validation to reject.
    guard_results = guards.run_all(repo, report, touched, route)
    for gok, gmsg in guard_results:
        print(f"  guard {'ok  ' if gok else 'FAIL'} {gmsg[:200]}")
    failed_guards = [m for g, m in guard_results if not g]
    if failed_guards:
        subprocess.run([ABT, "comment", str(a.id), "--body",
                        "Minion proposed a fix that did not clear the standing guards, so "
                        "it was NOT committed. These are rules earlier runs paid for; see "
                        "Kelvin/tools/qarlos/lessons.py.\n\n- " + "\n- ".join(failed_guards)])
        return 1

    # Gate against where the run STARTED. changed_records_gate defaults to --base HEAD,
    # so once the agent has committed its own work the changed files no longer look
    # changed and the gate validates the wrong file — in ABT #524 it reported on
    # mosfets.ndjson while the diode repair it was supposed to check sailed past.
    gates = [list(g) for g in cfg["gates"]]
    if self_committed and head_before:
        for g in gates:
            if any("changed_records_gate" in part for part in g):
                g.extend(["--base", head_before])
    ok, gate_out = run_gates(repo, gates)
    print(f"  gates: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(gate_out[-2000:])
        subprocess.run([ABT, "comment", str(a.id), "--body",
                        "Minion proposed a fix but the repo gates FAILED, so it was not "
                        f"committed.\n\n{gate_out[-3000:]}"])
        return 1

    if self_committed:
        # The agent already landed it, so commit_only never sees it and the pathspec
        # protection did not apply. Verifying HEAD is the one check still available.
        code_touched = [f for f in touched if not DATA_ONLY.match(f)]
        chg = cfg.get("clean_head_gates")
        if code_touched and chg:
            hok, hmsg = guards.verify_clean_head(repo, chg)
            print(f"  clean-HEAD (agent self-committed): {'PASS' if hok else 'FAIL'}")
            if not hok:
                print(hmsg[-1500:])
                subprocess.run([ABT, "comment", str(a.id), "--body",
                                "CRITICAL: the agent committed its own fix and HEAD does "
                                "NOT pass its own gates from a clean checkout.\n\n"
                                f"{hmsg[-2500:]}"])
                return 4

    if a.commit and touched and not self_committed:
        msg = (f"{report.get('summary','fix')}\n\n"
               f"Root cause: {report.get('root_cause','')}\n"
               f"Wider impact: {report.get('wider_impact','')}\n\n"
               f"Found by Qarlos, fixed by Minion. ABT #{a.id}.\n")
        # Pathspec-limited: `git add` + a later `git commit` commits the INDEX, which
        # several agents share. That is how a staged revert of a 461 MB data file once
        # rode into a commit meant to touch only C++ sources (lessons: the-index-is-shared).
        cok, cmsg = guards.commit_only(repo, touched, msg)
        print(f"  commit: {cmsg}")
        if not cok:
            subprocess.run([ABT, "comment", str(a.id), "--body",
                            f"Minion's fix passed the gates but could not be committed "
                            f"safely: {cmsg}"])
            return 1

        # Now that it is IN HEAD, prove HEAD itself stands up — but only for code changes.
        # A working tree that passes proves nothing about HEAD; on 2026-08-02 a tree passed
        # 211/211 while a clean checkout of the same HEAD failed 2 cases.
        code_touched = [f for f in touched if not DATA_ONLY.match(f)]
        chg = cfg.get("clean_head_gates")
        if code_touched and chg:
            hok, hmsg = guards.verify_clean_head(repo, chg)
            print(f"  clean-HEAD: {'PASS' if hok else 'FAIL'}")
            if not hok:
                print(hmsg[-1500:])
                subprocess.run([ABT, "comment", str(a.id), "--body",
                                "CRITICAL: Minion's fix is committed and HEAD does NOT pass "
                                "its own gates from a clean checkout. The working tree hid "
                                "it. Stop draining and repair main before landing anything "
                                f"else.\n\n{hmsg[-2500:]}"])
                return 4      # distinct code: drain.py must halt, not continue
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
