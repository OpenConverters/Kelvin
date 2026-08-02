#!/usr/bin/env python3
"""Lessons — the operating rules Qarlos and Minion have PAID FOR, in a form they read.

WHY THIS FILE EXISTS. The absolute rules in minion.py's system prompt are the ones whose
violation is obviously catastrophic: never edit a schema, never delete a record, never
weaken a gate. Those were easy to write down because breaking them is visibly wrong.

This file holds the other kind — the rules nobody would think to state until a specific
day went badly. Each one below cost real damage or real hours, and each names the incident
so a future reader can judge whether it still applies rather than obeying it on faith. A
rule with no incident behind it does not belong here; it is speculation, and speculation in
an autonomous fixer's prompt is how you get confident wrong behaviour.

HOW IT IS USED. render() produces the block appended to Minion's system prompt, and a
shorter subset goes to Qarlos (auditor-relevant only). Adding a lesson here changes what
every subsequent autonomous run knows, which is the point: this is the loop's memory, and
it is version-controlled, greppable and reviewable, unlike a rule that lives in one
session's head.

WHEN TO ADD ONE. After any run — autonomous or human — where the outcome was wrong in a
way the existing rules did not prevent. Write what a reader has to BELIEVE to make the
mistake, not just what to do instead. Keep `incident` concrete and numeric; that is what
makes the rule survive a future reader who disagrees with it.

Several lessons here are mechanically enforced in guards.py. Where that is so, the lesson
says which check enforces it. A rule stated in a prompt is a hope; a rule in guards.py is
a fact. Prefer moving a lesson into guards.py over restating it more loudly.
"""
from dataclasses import dataclass, field
from typing import List


@dataclass(frozen=True)
class Lesson:
    id: str
    audience: str          # "minion" | "qarlos" | "both"
    rule: str              # the imperative, one or two sentences
    incident: str          # what actually happened, with numbers
    apply: str             # what to do differently, concretely
    enforced_by: str = ""  # guards.py function, if this is checked rather than trusted
    tags: List[str] = field(default_factory=list)


LESSONS: List[Lesson] = [

    # ---- calibration -------------------------------------------------------
    Lesson(
        id="calibrate-before-shipping",
        audience="both",
        rule="Before a NEW OR TIGHTENED detection rule is allowed to call anything invalid, "
             "measure how often it fires across the WHOLE live corpus. A rule that fires on "
             "correct data does not find defects, it manufactures them.",
        incident="2026-08-02, the circuit validator. Three draft rules were written from "
                 "first principles and two were wrong on real data: a floating-node check "
                 "that ignored ports fired on 38.84% of 19,533 bricks — including all 7,554 "
                 "connector pin-field bricks, which had just been PROVEN correct by "
                 "simulating every one of them in ngspice. A 1 F capacitance ceiling fired "
                 "on 87 supercapacitor bricks; an 851617031001_100F is a 100 F cell and "
                 "commercial modules reach ~10 kF. Both were caught only because the rules "
                 "were run corpus-wide before being given a severity. Earlier precedent: a "
                 "connector screen whose physics was correct still fired on 45-85% of real "
                 "parts.",
        apply="Run the candidate over every record and report the fire rate before choosing "
              "a severity. IMPOSSIBLE is for FORWARD guards — it should invalidate ~nothing "
              "that already exists. A rule that fires on thousands of live records is either "
              "wrong or is a SUSPICIOUS-grade observation, never an IMPOSSIBLE. If a rule "
              "must fire retroactively, say so explicitly in the ticket with the count.",
        enforced_by="guards.check_rule_calibration",
        tags=["validator", "severity"],
    ),
    Lesson(
        id="suspicious-when-you-were-wrong-once",
        audience="minion",
        rule="If an earlier formulation of a rule was badly wrong, ship the corrected "
             "version as SUSPICIOUS even when the corrected fire rate is zero.",
        incident="2026-08-02. The corrected floating-node rule fires on 0 of 19,533 bricks, "
                 "and a floating node genuinely IS a singular matrix — the strongest case "
                 "for IMPOSSIBLE there is. It was still shipped SUSPICIOUS, because the "
                 "first formulation of that same rule was wrong about a third of the corpus.",
        apply="One bad formulation is evidence about the author's grasp of the domain, not "
              "just about that draft. Promote to IMPOSSIBLE later, when a real failure is "
              "traced to something the rule flagged.",
        tags=["severity"],
    ),

    # ---- gates that are not running ---------------------------------------
    Lesson(
        id="a-stale-artifact-shrinks-a-gate-silently",
        audience="minion",
        rule="Before trusting a gate, prove the binary or module it runs is BUILT FROM "
             "CURRENT SOURCE. A stale artifact does not fail — it quietly runs fewer checks "
             "and reports clean.",
        incident="2026-08-02, ABT #397. The validator was rebuilt in validator/build-ninja "
                 "with new circuit checks and 13 new tests, all green. blade_gate.py imports "
                 "validator/build — a DIFFERENT tree, holding a 4.5-hour-old .so with none of "
                 "the new checks compiled in. hasattr(module, 'validate_circuit') would "
                 "simply have been False and the gate would have run a subset of its rules "
                 "while reporting success. The two trees did not even share a CMake generator.",
        apply="Probe for the capability you are about to rely on and fail loudly if it is "
              "absent, rather than degrading. Compare artifact mtime against its sources. "
              "One build tree per project; if you find two, say so.",
        enforced_by="guards.check_validator_freshness",
        tags=["gate", "build"],
    ),
    Lesson(
        id="an-ungated-file-is-invisible",
        audience="both",
        rule="A data file with no gate mapping is not 'passing' — it is UNEXAMINED. Absence "
             "of findings from a file nothing checks is not evidence of quality.",
        incident="2026-08-02. data/circuits.ndjson had no entry in changed_records_gate's "
                 "FAMILIES table, so the gate exited 2 ('NO FAMILY MAPPING — refusing to "
                 "pass') on any change touching it. Correct behaviour, and the effect was "
                 "that 13,615 CIAS bricks had never been checked by anything but a schema "
                 "pass for the file's entire existence. The first real run found 50 "
                 "pre-existing defects, including 7 bricks storing magnetizingInductance 0.",
        apply="Periodically enumerate every data file and assert each is covered by a gate. "
              "Treat 'no findings' from an unmapped file as a coverage hole to report, never "
              "as a clean result.",
        enforced_by="gate_coverage.py",
        tags=["gate", "coverage"],
    ),
    Lesson(
        id="gates-do-not-subsume-each-other",
        audience="both",
        rule="Shape, structure, emittability and physics are four independent questions. "
             "Passing one says nothing about the others.",
        incident="2026-08-02. Bricks 750811612 and 750315229 each carry a 0 F capacitor. "
                 "They pass JSON Schema, pass the structural validator, and lower cleanly to "
                 "a SPICE card — 'CCpri2 Cpri2__pi 1 0'. A zero-farad capacitor is a legal "
                 "JSON number and a legal SPICE card; it is simply not an element. Nothing "
                 "in the stack saw them until a physics check for circuits existed. "
                 "Conversely 7 bricks with a 0 H inductor were caught ONLY by the emitter, "
                 "which the schema and the part-level physics validator both passed.",
        apply="When a defect slips through, ask which of the four gates SHOULD have owned it "
              "and add it there, rather than widening whichever gate happens to be nearest.",
        tags=["gate"],
    ),

    # ---- verification hygiene ---------------------------------------------
    Lesson(
        id="verify-from-clean-head",
        audience="minion",
        rule="A passing working tree proves nothing about what you are committing. Verify "
             "from a clean checkout of HEAD.",
        incident="2026-08-02, ABT #542. Tests for GEN_PACKAGE_MOUNT and GEN_FABRICATED_MPN "
                 "were committed while their implementation sat uncommitted in a working "
                 "tree. That tree passed 211/211. A clean detached worktree at the same HEAD "
                 "failed 2 cases and 9 assertions. The breakage was invisible to its author "
                 "precisely BECAUSE their tree was the one holding the missing half. The same "
                 "thing happened a second time the same day in the other direction.",
        apply="git worktree add --detach <tmp> HEAD, build and run the suite there, then "
              "remove it. Point FetchContent at an existing _deps to avoid a refetch.",
        enforced_by="guards.verify_clean_head",
        tags=["git", "gate"],
    ),
    Lesson(
        id="the-index-is-shared",
        audience="minion",
        rule="Never `git add` and then commit as a separate later step. Commit with an "
             "explicit pathspec so only your files can land.",
        incident="2026-08-02, twice in one day. (a) Another session's commit swept up "
                 "in-progress test additions that had no implementation, leaving HEAD unable "
                 "to compile. (b) A commit intending to touch only validator sources also "
                 "carried a revert of data/circuits.ndjson that another session had staged "
                 "in the shared index, silently removing 7,554 verified bricks (a 461 MB LFS "
                 "object) from HEAD. In a third case an index was cleared mid-operation and "
                 "a commit failed with 'no changes added'. `git commit` commits the INDEX, "
                 "not your intentions, and several agents share it.",
        apply="Use `git commit -- <paths>`, or re-read `git diff --cached --name-only` "
              "IMMEDIATELY before committing and abort if it differs from what you touched. "
              "Deriving 'my files' from a before/after dirty-set diff is not safe either: a "
              "concurrent process dirtying a file during your run makes it look like yours.",
        enforced_by="guards.commit_only",
        tags=["git", "concurrency"],
    ),
    Lesson(
        id="a-reported-count-is-a-floor",
        audience="both",
        rule="The number of bad records a detector reports is a LOWER BOUND, not the "
             "population. Re-measure with a detector that does not stop early.",
        incident="2026-08-02, ABT #511. The ticket said 7 bricks carried a zero-henry "
                 "inductor, because the count came from the CIAS lowering, which throws on "
                 "the FIRST bad element in a brick and stops. A per-record physics pass over "
                 "the same data found each of those bricks holds FOUR — 28, not 7 — and "
                 "surfaced 2 further bricks in the same class that lowered cleanly and had "
                 "never appeared in any failure list.",
        apply="Before reporting a population fixed, re-run a detector that enumerates ALL "
              "violations per record. If population_after is 0 but the detector short-"
              "circuits, you have proven nothing.",
        tags=["population"],
    ),
    Lesson(
        id="find-the-criterion-not-the-exceptions",
        audience="both",
        rule="When correct data trips a check, do not add the cases to an exception list. "
             "Find the property that distinguishes them and test THAT.",
        incident="2026-08-02. 324 of 7,554 pin-field bricks tripped a crosstalk "
                 "monotonicity check. All 324 were the finite-array edge effect — the "
                 "outermost conductor is unshielded on one side. Rather than pin 324 names, "
                 "the check now asks whether the brick's OWN coupling capacitance also rises "
                 "at the violating pin: if the numbers explain the behaviour it is the edge "
                 "effect, if coupling rises while capacitance falls it is a real regression. "
                 "That criterion held for all 324 with no counterexample, and it survived "
                 "the corpus growing to 13,264 bricks with 5,710 multi-row grids, which an "
                 "exception list would not have.",
        apply="An exception list is a bug that has been written down. It goes stale the "
              "moment the data grows.",
        tags=["validator"],
    ),

    # ---- what to fix -------------------------------------------------------
    Lesson(
        id="fix-the-producer",
        audience="minion",
        rule="If bad records came from an importer, scraper or generator, fixing the records "
             "is HALF the job. The producer will reintroduce them on its next run.",
        incident="Open tickets as of 2026-08-02 that are all producer bugs: #544 (a Murata "
                 "importer placeholder wrote tcc {nominal: 0} on 7,146 parts), #529 (a TDK "
                 "import GUESSED windingStyle/material/shielded), #534 (958 records carry a "
                 "subType the extractor invented), #286, #281. Their record counts understate "
                 "them, because the count regrows.",
        apply="Name the producer in root_cause and change it in the same fix. If you cannot "
              "reach it, say so explicitly in wider_impact and file a separate ticket against "
              "it — do not report the population fixed as though it will stay fixed.",
        tags=["root-cause"],
    ),
    Lesson(
        id="provenance-must-be-queryable",
        audience="both",
        rule="A verification result stored as prose is not a verification result. It must be "
             "a field a query can filter on.",
        incident="2026-08-02. 236,235 rows had citations checked against the actual source "
                 "documents — real work — and the outcome was written by appending a note to "
                 "the provenance entry's `sourceName` string. Grepping 754,113 records across "
                 "five catalogues for a `verdict` field returns ZERO. A 48,000-record sample "
                 "shows 60.3% carry a bare citation with no verification statement, 20.7% say "
                 "explicitly NOT verified, 19.0% claim verified — and that ratio is only "
                 "recoverable by regex over a display string. 'Give me only verified parts' "
                 "is not currently answerable.",
        apply="When recording the outcome of a check, put it in a structured field. If the "
              "schema has no such field, report the gap (ABT #391) — do not encode it in a "
              "name, a description or a comment.",
        tags=["provenance"],
    ),

    Lesson(
        id="the-agent-commits-its-own-work",
        audience="minion",
        rule="Do not infer 'what this run changed' from the dirty working tree alone. An "
             "agent that commits its own fix leaves the tree clean, and every check that "
             "hangs off that list then silently does nothing.",
        incident="2026-08-02, the first production run with these lessons loaded. It "
                 "repaired 183 diode records, fixed two importers and added a Blade Runner "
                 "rule — genuinely good work, visibly shaped by the lessons — and the "
                 "harness reported 'Files: (none)', because the agent had committed before "
                 "returning. Consequences: the rule-calibration guard saw no validator "
                 "paths and did not fire on a run that ADDED A RULE; the pathspec commit "
                 "never ran; the clean-HEAD check never ran; and changed_records_gate, "
                 "which defaults to --base HEAD, found nothing changed and validated "
                 "mosfets.ndjson while the diode repair it existed to check went ungated.",
        apply="Compute the change set as (still-dirty files) UNION (files in "
              "head_before..head_now), and pass --base <head_before> to any gate that "
              "diffs against HEAD. A guard that quietly evaluates an empty list is "
              "indistinguishable from a guard that passed.",
        enforced_by="minion.py (touched/self_committed)",
        tags=["harness", "git"],
    ),

    Lesson(
        id="cannot-run-is-not-could-not-fix",
        audience="minion",
        rule="An agent that could not be REACHED has said nothing about the ticket. Never "
             "record an infrastructure failure as a verdict on the work.",
        incident="2026-08-02 15:28. The nightly drain exhausted the account's weekly API "
                 "limit and then marched through 11 more tickets, getting an empty response "
                 "each time and writing 'Minion could not fix this. Blocked by: (unstated)' "
                 "on every one. Eleven live defects now carry what reads as a considered "
                 "assessment and is really a fact about billing. The run reported "
                 "'minion_failed 11 | resolved 3' — the 11 and the 3 are not the same kind "
                 "of number and were tallied as though they were.",
        apply="Detect quota/transport failure explicitly and exit with a distinct code; the "
              "orchestrator must HALT rather than spend the queue. Leave the ticket "
              "untouched — silence is more honest than a false verdict.",
        enforced_by="minion.INFRA_FAILURE / drain 'cannot_run'",
        tags=["harness", "reporting"],
    ),

    # ---- reporting ---------------------------------------------------------
    Lesson(
        id="do-not-starve-the-newest-findings",
        audience="both",
        rule="When a report is capped, give each finding CLASS its own budget. A single "
             "shared cap is spent by whatever appears first in file order.",
        incident="2026-08-02. The circuits gate found two bricks that only its newest check "
                 "could see, and printed neither: 47 lowering failures and 3 structural ones "
                 "came earlier in file order and consumed all 20 report slots. The counts "
                 "were right and the evidence was invisible.",
        apply="Per-class report budgets, and always print the TOTAL per class even when the "
              "examples are capped.",
        tags=["reporting"],
    ),
    Lesson(
        id="say-what-you-did-not-check",
        audience="both",
        rule="State the boundary of what was verified. A sample is not the corpus and a "
             "subset is not the population.",
        incident="2026-08-02. A DB assessment quoting '157 open tickets' inside a section "
                 "about data implied all 157 were data defects; 48 were, the rest belonged to "
                 "~19 other repos. Separately, a provenance figure came from a 48,000-record "
                 "sample of ~960,000 and had to be labelled as such to be honest.",
        apply="Give the denominator. If you sampled, say the sample size and that it is one.",
        tags=["reporting"],
    ),
]


def _fmt(l: Lesson) -> str:
    s = f"[{l.id}] {l.rule}\n    WHY: {l.incident}\n    DO: {l.apply}"
    if l.enforced_by:
        s += f"\n    ENFORCED: this is checked mechanically by {l.enforced_by} — a run that "
        s += "violates it is rejected, not warned."
    return s


def render(audience: str) -> str:
    """The prompt block for 'minion' or 'qarlos'."""
    picked = [l for l in LESSONS if l.audience in (audience, "both")]
    if not picked:
        return ""
    head = (
        "LESSONS PAID FOR IN PREVIOUS RUNS. These are not style preferences. Each one below "
        "is a specific way autonomous work on this corpus has already gone wrong, with the "
        "incident attached so you can judge it rather than obey it. Where a lesson says "
        "ENFORCED, a check will reject your run for violating it, so treat those as hard.\n"
    )
    return head + "\n\n".join(_fmt(l) for l in picked)


def ids() -> List[str]:
    return [l.id for l in LESSONS]


if __name__ == "__main__":
    import sys
    who = sys.argv[1] if len(sys.argv) > 1 else "minion"
    print(render(who))
    print(f"\n--- {len([l for l in LESSONS if l.audience in (who, 'both')])} lessons for "
          f"{who}, {len(LESSONS)} total ---")
