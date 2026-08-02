#!/usr/bin/env python3
"""Guards — the lessons that are CHECKED rather than told.

A rule in a system prompt is a hope. Minion is a capable model working unattended, and the
failure modes below are ones where a careful operator still gets it wrong, because the
mistake looks exactly like success from inside the run. Those belong here.

Each function maps to a lesson in lessons.py and is named there, so a reader of either file
can find the other. Everything returns (ok, message): a guard NEVER fixes silently and never
raises past the caller, because a guard that aborts a run in a way nobody sees is its own
failure mode.

Scope note: these are cheap, local checks. They do not replace the repo gates in minion.py
(schema + Blade Runner + Catch2); they cover the ways a run can pass those gates and still
be wrong.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

Result = Tuple[bool, str]


def _git(repo: Path, *args: str, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *args],
                          capture_output=True, text=True, timeout=timeout)


# ---------------------------------------------------------------------------
# lessons: the-index-is-shared
# ---------------------------------------------------------------------------
def commit_only(repo: Path, paths: Sequence[str], message: str) -> Result:
    """Commit EXACTLY `paths` and nothing else.

    `git add X && git commit` is not safe here: git commit commits the INDEX, and several
    agents share this working tree. On 2026-08-02 that swept a staged revert of a 461 MB
    LFS data file into a commit that meant to touch only C++ sources, silently removing
    7,554 verified records from HEAD. A pathspec-limited commit cannot do that, because it
    ignores whatever else is staged.

    Also refuses to commit nothing, which otherwise looks like success.
    """
    if not paths:
        return False, "commit_only: refusing to commit an empty pathspec"

    missing = [p for p in paths if not (repo / p).exists()]
    # A deleted file is a legitimate thing to commit, so only flag paths git does not know.
    for p in list(missing):
        if _git(repo, "ls-files", "--error-unmatch", p).returncode == 0:
            missing.remove(p)
    if missing:
        return False, f"commit_only: paths do not exist and are untracked: {missing}"

    staged_before = {l for l in _git(repo, "diff", "--cached", "--name-only").stdout.split()}
    foreign = staged_before - set(paths)

    # -- the commit itself: pathspec-limited, so `foreign` cannot ride along --
    p = _git(repo, "commit", "-q", "-m", message, "--", *paths, timeout=600)
    if p.returncode != 0:
        return False, f"commit_only: git commit failed: {(p.stderr or p.stdout)[:300]}"

    landed = {l for l in _git(repo, "show", "--pretty=", "--name-only", "HEAD").stdout.split()}
    unexpected = landed - set(paths)
    if unexpected:
        return False, (f"commit_only: HEAD contains files that were not in the pathspec: "
                       f"{sorted(unexpected)} — investigate before pushing")
    note = ""
    if foreign:
        note = (f" (left {len(foreign)} file(s) staged by another process uncommitted: "
                f"{sorted(foreign)[:5]})")
    return True, f"committed {len(landed)} file(s){note}"


# ---------------------------------------------------------------------------
# lessons: verify-from-clean-head
# ---------------------------------------------------------------------------
def verify_clean_head(repo: Path, gates: Iterable[Sequence[str]],
                      extra_cmake_args: Sequence[str] = (),
                      timeout: int = 5400) -> Result:
    """Run `gates` against a detached worktree at HEAD, not against the working tree.

    The working tree is exactly where a missing half hides: on 2026-08-02 a tree passed
    211/211 while a clean checkout of the SAME HEAD failed 2 cases and 9 assertions,
    because the implementation those tests needed was uncommitted. The author could not
    have seen it without doing this.

    Returns (True, ...) only if every gate exits 0 in the clean tree.
    """
    if _git(repo, "rev-parse", "HEAD").returncode != 0:
        return False, "verify_clean_head: not a git repo"

    tmp = Path(tempfile.mkdtemp(prefix="cleanhead_"))
    wt = tmp / "wt"
    add = _git(repo, "worktree", "add", "-q", "--detach", str(wt), "HEAD", timeout=600)
    if add.returncode != 0:
        shutil.rmtree(tmp, ignore_errors=True)
        return False, f"verify_clean_head: worktree add failed: {(add.stderr or '')[:200]}"

    try:
        out: List[str] = []
        for g in gates:
            cmd = [a.replace("{WT}", str(wt)) for a in g] + list(extra_cmake_args)
            try:
                p = subprocess.run(cmd, cwd=str(wt), capture_output=True, text=True,
                                   timeout=timeout)
            except subprocess.TimeoutExpired:
                return False, f"verify_clean_head: `{' '.join(cmd)}` timed out"
            tail = ((p.stdout or "")[-800:] + (p.stderr or "")[-400:]).strip()
            out.append(f"$ {' '.join(cmd)}\n{tail}")
            if p.returncode != 0:
                return False, ("verify_clean_head: HEAD FAILS ITS OWN GATES — the working "
                               "tree was hiding it.\n" + "\n\n".join(out))
        return True, "clean HEAD passes:\n" + "\n\n".join(out)
    finally:
        _git(repo, "worktree", "remove", "--force", str(wt), timeout=300)
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
# lessons: a-stale-artifact-shrinks-a-gate-silently
# ---------------------------------------------------------------------------
def check_validator_freshness(tas: Path) -> Result:
    """The Blade Runner module the gates import must be newer than its sources.

    A stale .so does not fail. It runs fewer checks and reports clean — on 2026-08-02 the
    module blade_gate imports was 4.5 hours behind and simply did not contain the checks
    that had just been written and tested in a second build tree.

    Also reports a second build tree if one exists, because that is the mechanism (#397).
    """
    build = tas / "validator" / "build"
    src = tas / "validator" / "src"
    inc = tas / "validator" / "include"
    sos = list(build.glob("tas_validator*.so"))
    if not sos:
        return False, (f"check_validator_freshness: no tas_validator*.so in {build}. The "
                       f"gates import that path; build it with "
                       f"`cmake --build {build} -j8`.")
    so_mtime = max(s.stat().st_mtime for s in sos)

    newest_src, newest_name = 0.0, ""
    for root in (src, inc):
        if not root.is_dir():
            continue
        for f in root.rglob("*"):
            if f.suffix in (".cpp", ".hpp", ".h") and f.stat().st_mtime > newest_src:
                newest_src, newest_name = f.stat().st_mtime, str(f.relative_to(tas))
    if newest_src > so_mtime:
        age = (newest_src - so_mtime) / 60.0
        return False, (f"check_validator_freshness: {newest_name} is {age:.0f} min NEWER "
                       f"than {sos[0].name}. The gate would import a module missing those "
                       f"checks and report clean. Rebuild: cmake --build {build} -j8")

    others = [d for d in (tas / "validator").glob("build*")
              if d.is_dir() and d.name != "build" and list(d.glob("tas_validator*.so"))]
    if others:
        return True, (f"validator is current, BUT a second build tree exists "
                      f"({', '.join(d.name for d in others)}) and only `build/` is imported "
                      f"by the gates — ABT #397. Anything built there is invisible.")
    return True, "validator module is newer than its sources"


# ---------------------------------------------------------------------------
# lessons: calibrate-before-shipping
# ---------------------------------------------------------------------------
VALIDATOR_RULE_PATHS = re.compile(r"validator/(src|include)/.*\.(cpp|hpp)$")


def touches_detection_rules(paths: Iterable[str]) -> List[str]:
    return [p for p in paths if VALIDATOR_RULE_PATHS.search(p)]


def check_rule_calibration(report: dict, touched: Iterable[str]) -> Result:
    """If a run changed detection rules, it must report a corpus-wide fire rate.

    Two of three rules drafted from first principles on 2026-08-02 were wrong on real data
    — one fired on 38.84% of the corpus, including records that had just been proven
    correct by simulation. Neither was caught by reasoning; both were caught by counting.

    This does not judge the number. It refuses to let a rule change land with no number at
    all, which is the state in which a retroactive mass-invalidation ships unnoticed.
    """
    changed = touches_detection_rules(touched)
    if not changed:
        return True, ""
    cal = (report.get("calibration") or "").strip()
    if not cal:
        return False, ("check_rule_calibration: this run changed detection rules "
                       f"({changed}) but reported no corpus-wide fire rate. Run the new or "
                       "tightened rule over every record and report, per rule, how many "
                       "existing records it fires on. IMPOSSIBLE is for forward guards: a "
                       "rule that invalidates live data needs that stated explicitly with "
                       "the count, not discovered later.")
    if not re.search(r"\d", cal):
        return False, (f"check_rule_calibration: 'calibration' contains no numbers: "
                       f"{cal[:160]!r}. A fire rate is a count over a denominator.")
    return True, f"rule change calibrated: {cal[:200]}"


# ---------------------------------------------------------------------------
# lessons: fix-the-producer
# ---------------------------------------------------------------------------
PRODUCER_HINT = re.compile(
    r"import|scrape|scraper|extract|generat|ingest|migrat|backfill|back-fill|harvest",
    re.I)


def check_producer_addressed(report: dict, touched: Iterable[str]) -> Result:
    """If the stated root cause is a producer, something should have changed in one.

    Advisory, not fatal: the producer is sometimes outside the repo, and saying so is a
    legitimate outcome. What is not legitimate is reporting a population fixed while the
    thing that produced it still runs — #544 regrows 7,146 records, #534 regrows 958.
    """
    cause = f"{report.get('root_cause','')} {report.get('summary','')}"
    if not PRODUCER_HINT.search(cause):
        return True, ""
    if any(PRODUCER_HINT.search(p) for p in touched):
        return True, "producer changed alongside the records"
    impact = (report.get("wider_impact") or "").lower()
    if any(w in impact for w in ("importer", "producer", "script", "regenerat", "ticket",
                                 "abt #", "reintroduce")):
        return True, "producer not reachable here, but named in wider_impact"
    return False, ("check_producer_addressed: root_cause names a producer "
                   f"({cause[:120]!r}) but no producer file was changed and wider_impact "
                   "does not say what stops it reintroducing the defect. Fix it, or file a "
                   "ticket against it and name that ticket — a population 'fixed' while its "
                   "producer still runs regrows.")


# ---------------------------------------------------------------------------
def run_all(repo: Path, report: dict, touched: Sequence[str], route: str) -> List[Result]:
    """Every guard that applies before a fix is allowed to land. Order is cheapest-first."""
    out: List[Result] = []
    ok, msg = check_rule_calibration(report, touched)
    out.append((ok, f"rule-calibration: {msg}" if msg else "rule-calibration: n/a"))
    ok2, msg2 = check_producer_addressed(report, touched)
    out.append((ok2, f"producer: {msg2}" if msg2 else "producer: n/a"))
    if route == "tas":
        ok3, msg3 = check_validator_freshness(repo)
        out.append((ok3, f"validator-freshness: {msg3}"))
    return out


if __name__ == "__main__":
    import sys
    tas = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / "PSMA" / "TAS"
    for name, res in (("validator-freshness", check_validator_freshness(tas)),):
        ok, msg = res
        print(f"[{'ok ' if ok else 'FAIL'}] {name}: {msg}")
