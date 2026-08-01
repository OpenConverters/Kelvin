#!/usr/bin/env python3
"""Qarlos — the standing auditor of Kelvin's cross-reference.

Every run: pull a couple of random parts per component family out of the Kelvin
shards, run the REAL cross-reference on each, have an LLM read the verdicts as an
application engineer would, and file an ABT ticket for anything that is actually
wrong. Deterministic engine, judged output, evidence attached.

WHAT MAKES THIS AN AUDIT AND NOT A SECOND OPINION. probe.mjs imports
web/src/crossref.js — the same module the site renders from — and runs it against
the same kelvin.js WASM the browser loads. There is no re-implementation of the
ranking rules to drift out of sync, so a finding here is a finding about what
users actually see. (crossref.js was extracted out of CrossRefView.vue for exactly
this; the view now imports it too.)

THE JUDGE IS DELIBERATELY RELUCTANT. A false ABT costs a human triage, and an
auditor that cries wolf gets muted, which is worse than no auditor. So the prompt
demands a concrete, checkable defect quoting the two datasheet numbers involved,
treats "unverified"/"no substitute" as legitimate honest answers rather than
failures, and defaults to OK when unsure. Findings are deduped by signature
against tickets already on the board.

The LLM runs through the `claude` CLI (Claude Code subscription), invoked by
ABSOLUTE path: a cron job does not get the interactive shell's PATH, which is
precisely how the librarian's nightly silently yielded nothing for weeks.

  qarlos.py --per-family 2                 # audit, print findings, file nothing
  qarlos.py --per-family 2 --file          # ... and file ABT tickets
  qarlos.py --seed 4242 --families mosfet  # reproduce a previous run exactly
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
KELVIN = HERE.parent.parent
PROBE = HERE / "probe.mjs"
STATE = Path.home() / ".qarlos"
SEEN = STATE / "reported.json"

# Absolute paths: cron has neither the login shell's PATH nor its aliases.
CLAUDE = os.environ.get("QARLOS_CLAUDE") or str(Path.home() / ".local/bin/claude")
ABT = os.environ.get("QARLOS_ABT") or str(Path.home() / ".local/bin/abt")
NODE = os.environ.get("QARLOS_NODE") or shutil.which("node") or "/usr/bin/node"

SYSTEM = """You are a power-electronics application engineer auditing a component \
cross-reference tool. You are the last check before an engineer swaps a part into a \
production board, so a wrong substitute you wave through is a field failure.

You are equally accountable for false alarms. Every issue you raise costs a human a \
triage cycle. Raise ONLY defects you can state concretely and back with the numbers in \
front of you.

These are NOT defects:
  - "unverified" params, or a refusal to recommend anything. The tool is designed to be
    honest about missing data; that is correct behaviour, not a bug.
  - original_hard_gate_specs_known being true while other fields are null. It reports on
    the PRIMARY value and the HARD-GATE specs only, which is its whole purpose; a null
    somewhere else is not a contradiction of it.
  - A substitute in a different package, when the tool already says so in its notes.
  - Over-dimensioned parts (higher voltage/current rating than the original) unless the
    tool called that a downgrade, or the over-dimensioning breaks the circuit (e.g. a
    much higher Vf, Rds(on), or ESR).
  - Ranking order you merely disagree with. Only flag order when a candidate the tool
    ranked as 'recommended'/'drop_in' is clearly WORSE than one it ranked below.

These ARE defects:
  - A verdict that contradicts the numbers (e.g. params say a rating is lower than the
    original's, yet the candidate is graded a clean drop-in).
  - A parameter verdict that is wrong given the two values (pass where the substitute is
    worse on a spec that matters, fail where it is actually fine).
  - A substitute from a different device class or technology presented as equivalent
    (a GaN part for a Si one with no note, a class-2 MLCC for a C0G, a bead for an
    inductor).
  - A note whose prose contradicts the numeric table.
  - A physically impossible or nonsensical value reaching the user.

Each case gives you three views of the same parts, and WHICH ONE is wrong decides who
fixes it, so say which:
  - "catalogue": the raw TAS record itself holds a wrong or impossible value (a units
    error, a value no real device has). The ranking behaved correctly on bad input.
  - "extraction": the raw record is right but the shard row Kelvin built from it
    disagrees with it (a field read wrongly, scaled wrongly, or dropped).
  - "ranking": raw and extracted agree and are sane, but the verdict, grade or note
    drawn from them is wrong.
Bad catalogue data is fully in scope — finding it is part of the job, not a distraction.

YOU ARE LOOKING AT A SAMPLE. You were given one part because that is how sampling works,
but a catalogue defect is almost never one record — a units error is whatever the importer
wrote that day. So for defect_in "catalogue" you MUST also describe every record that has
the same defect, as a declarative query. Fixing the one part you were shown, and leaving
its siblings, is the failure this field exists to prevent.

  "population_query": {"file": "<capacitors.ndjson|mosfets.ndjson|connectors.ndjson|...>",
                       "all": [{"path": "<dotted path from the record root>",
                                "op": "<|<=|>|>=|==|!=|exists|startswith|contains|in",
                                "value": <literal>}, ...]}

Paths start at the raw record root, so they include the discriminator, e.g.
"capacitor.manufacturerInfo.datasheetInfo.electrical.esr" or
"semiconductor.mosfet.manufacturerInfo.datasheetInfo.electrical.totalGateCharge".
Use "[]" for "any element of this array", e.g. "…electrical.[].onResistance".

Make the query describe the DEFECT, not the one part: the conditions that make a record
wrong (an impossible value, in the technology where it is impossible), never the specific
MPN. It will be run over the whole catalogue and the count goes in the ticket, so a query
that is too broad slanders good records and one that is too narrow hides the problem.
Omit population_query for "extraction" and "ranking" — one code fix covers every part.

Reply with STRICT JSON, no prose outside it:
{"ok": true}
or
{"ok": false, "findings": [{"severity": "critical|high|medium|low",
  "defect_in": "catalogue|extraction|ranking",
  "title": "<one line>", "candidate": "<candidate MPN, or the original's MPN>",
  "what_is_wrong": "<what the tool said>",
  "evidence": "<the numbers that contradict it, quoting the raw record>",
  "expected": "<what it should have said>",
  "population_query": <the query, or null for code defects>}]}"""


VERIFY_SYSTEM = """You are verifying a claimed defect in a component cross-reference tool, and your job is to REFUTE it. The claim was made by another reviewer who saw the same data. Most claims that reach you are wrong in some detail, and filing a wrong one wastes an engineer's day.

Work only from the evidence given. Check, in order:
  1. Do the numbers the claim quotes actually appear in the data? If a quoted value is
     not there, or is misquoted, the claim is REFUTED.
  2. Is the claim's physics right? Check the arithmetic and the unit scaling yourself
     (SI base units throughout: F, H, ohm, V, A, C, Hz, m, s). A value that looks absurd
     may just be a unit you misread.
  3. Is `defect_in` right? "catalogue" needs the RAW record itself to be wrong.
     "extraction" needs the raw record and Kelvin's extracted row to DISAGREE — compare
     them field by field; if they agree, it is not extraction. "ranking" needs both to
     be fine and the verdict still wrong.
  4. Is it really a defect, or is it the tool being deliberately honest (an "unverified"
     param, a refusal to recommend, a declared caveat, a noted package change)?

Confirm ONLY a claim that survives all four. If the substance is right but `defect_in`
is wrong, confirm it and give the corrected value. When you cannot tell, REFUTE — a
missed defect costs one more audit cycle, a false one costs trust.

Reply with STRICT JSON, no prose outside it:
{"verdict": "confirmed|refuted", "reason": "<one or two sentences>",
 "defect_in": "catalogue|extraction|ranking", "severity": "critical|high|medium|low",
 "corrected_evidence": "<the evidence restated accurately, quoting real values>"}"""


def verify(case, finding, model, timeout):
    """Adversarial re-check against the RAW record. Only survivors get filed."""
    payload = {
        "claim": finding,
        "case": compact(case, max_candidates=6),
    }
    prompt = ("A reviewer claims the following defect. Try to refute it.\n\n```json\n"
              + json.dumps(payload, ensure_ascii=False, indent=1)
              + "\n```\n\nReply with the strict JSON described.")
    cmd = [CLAUDE, "-p", prompt, "--append-system-prompt", VERIFY_SYSTEM]
    if model:
        cmd += ["--model", model]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"verdict": "refuted", "reason": f"verifier timed out after {timeout}s"}
    if p.returncode != 0:
        return {"verdict": "refuted", "reason": (p.stderr or p.stdout)[-300:]}
    m = re.search(r"\{.*\}", (p.stdout or "").strip(), re.S)
    if not m:
        return {"verdict": "refuted", "reason": "verifier returned no JSON"}
    try:
        return json.loads(m.group(0))
    except json.JSONDecodeError as e:
        return {"verdict": "refuted", "reason": f"verifier JSON error: {e}"}


# Where a confirmed defect belongs. A units error in the catalogue is TAS's to fix; a
# wrong verdict or a mis-extracted field is Kelvin's.
DEFECT_ROUTE = {"catalogue": "tas", "extraction": "kelvin", "ranking": "kelvin"}


def run_probe(a):
    cmd = [NODE, str(PROBE), "--per-family", str(a.per_family),
           "--max-results", str(a.max_results)]
    if a.seed:
        cmd += ["--seed", str(a.seed)]
    if a.families:
        cmd += ["--families", a.families]
    if a.shards:
        cmd += ["--shards", a.shards]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if p.returncode != 0:
        sys.exit(f"probe failed:\n{p.stderr[-2000:]}")
    sys.stderr.write(p.stderr)
    cases = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
    seed = next((c["seed"] for c in cases if "seed" in c), a.seed)
    return cases, seed


def compact(case, max_candidates=6):
    """The evidence the judge sees: the original, and each candidate's verdicts."""
    o = case["original"]
    out = {
        "family": case["family"], "category": case["category"],
        "original": {k: v for k, v in o.items()
                     if not k.startswith("src") and k != "lineno"},
        "original_RAW_catalogue_record": case.get("originalRaw"),
        "original_spec_the_ranker_used": case.get("origSpec"),
        "target_manufacturers": case.get("targets"),
        "candidate_pool_size": case.get("poolScored"),
        # NAME THESE PRECISELY. `origVerified` means only that the original's PRIMARY
        # value and HARD-GATE specs are known — it is the honesty gate that decides
        # whether anything may rank as a clean 'recommended'. It does NOT claim every
        # field is populated. Labelling it "specs fully known" made the judge report a
        # contradiction on every single case: true alongside a spec block with nulls in
        # non-gate fields. That was the harness lying, not the tool.
        "original_hard_gate_specs_known": case.get("origVerified"),
        "original_missing_hard_gate_specs": case.get("missing"),
        "tool_caveat": case.get("caveat"),
        "candidates": [],
    }
    for c in case.get("ranked", [])[:max_candidates]:
        row = c.get("_row") or {}
        out["candidates"].append({
            "mpn": c.get("mpn"),
            "manufacturer": (c.get("_key") or "").split("␟")[0],
            "tool_status": c.get("status"), "tool_grade": c.get("grade"),
            "direction": c.get("direction"), "footprint": c.get("footprint"),
            "penalty": round(c.get("penalty", 0), 3),
            "param_verdicts": c.get("params"),
            "tool_notes": c.get("notes"),
            "candidate_specs_as_kelvin_extracted_them": {
                k: v for k, v in row.items()
                if not k.startswith("src") and k != "lineno"},
            "candidate_RAW_catalogue_record": c.get("_raw"),
        })
    return out


def judge(case, model, timeout):
    payload = json.dumps(compact(case), ensure_ascii=False, indent=1)
    prompt = ("Audit this cross-reference result.\n\n"
              "```json\n" + payload + "\n```\n\nReply with the strict JSON described.")
    cmd = [CLAUDE, "-p", prompt, "--append-system-prompt", SYSTEM]
    if model:
        cmd += ["--model", model]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"_judge_error": f"claude timed out after {timeout}s"}
    if p.returncode != 0:
        return {"_judge_error": (p.stderr or p.stdout)[-500:]}
    txt = (p.stdout or "").strip()
    m = re.search(r"\{.*\}", txt, re.S)
    if not m:
        return {"_judge_error": f"no JSON in reply: {txt[:300]}"}
    try:
        return json.loads(m.group(0))
    except json.JSONDecodeError as e:
        return {"_judge_error": f"bad JSON ({e}): {txt[:300]}"}


def signature(case, f):
    return f"{case['family']}|{case['original'].get('mpn')}|{f.get('candidate')}|{f.get('title','')[:60]}"


def load_seen():
    if SEEN.exists():
        try:
            return set(json.loads(SEEN.read_text()))
        except json.JSONDecodeError:
            return set()
    return set()


def save_seen(seen):
    STATE.mkdir(parents=True, exist_ok=True)
    SEEN.write_text(json.dumps(sorted(seen), indent=1))


SEV_TO_PRIORITY = {"critical": "critical", "high": "high", "medium": "medium",
                   "low": "low"}


def population_of(f):
    """How many records share this defect. A finding without this is a sample, not a scope."""
    q = f.get("population_query")
    if not q or not isinstance(q, dict) or not q.get("file"):
        return None
    try:
        from sweep import run as sweep_run
        return sweep_run(q, limit=8)
    except SystemExit as e:
        return {"error": str(e)}
    except Exception as e:  # noqa: BLE001
        return {"error": f"{type(e).__name__}: {e}"}


def file_abt(case, f, seed, dry, verdict):
    o = case["original"]
    repro = (f"node tools/qarlos/probe.mjs --seed {seed} "
             f"--families {case['family']} --per-family {os.environ.get('QARLOS_PF','2')}")
    where = f.get("defect_in", "ranking")
    route = DEFECT_ROUTE.get(where, "kelvin")
    pop = f.get("_population")
    if pop and pop.get("population") is not None:
        scope = (f"""
SCOPE — THIS IS NOT ONE RECORD
{pop['population']} of {pop['scanned']} records in {pop['file']} match this defect.
The part below is the sample that exposed it. A fix that repairs only that part leaves
{max(0, pop['population'] - 1)} others broken AND closes the ticket, which is worse than
not looking. Repair or quarantine the whole population.

  population query: {json.dumps(f.get('population_query'), ensure_ascii=False)}
  examples: {', '.join(pop.get('samples') or [])}
  recheck:  python3 tools/qarlos/sweep.py --query '{json.dumps(f.get('population_query'), ensure_ascii=False)}'
""")
    elif pop and pop.get("error"):
        scope = f"\nSCOPE  population query could not be run: {pop['error']}\n"
    elif where == "catalogue":
        scope = ("\nSCOPE  no population query was produced, so the blast radius of this "
                 "defect is UNKNOWN — check for siblings before closing.\n")
    else:
        scope = ("\nSCOPE  code defect: one fix in the extractor/ranker covers every part "
                 "in the family.\n")
    body = f"""Found by Qarlos, the standing cross-reference auditor.
{scope}

DEFECT IS IN: {where}   (catalogue = the TAS record itself; extraction = the shard row
Kelvin built from it; ranking = the verdict drawn from them)

ORIGINAL   {o.get('mpn')} ({o.get('manufacturer')}), family {case['family']}, category {case['category']}
CANDIDATE  {f.get('candidate')}
TARGETS    {', '.join(case.get('targets') or [])}
POOL       {case.get('poolScored')} candidates scored of {case.get('poolTotal')} pre-gate matches

WHAT THE TOOL SAID
{f.get('what_is_wrong')}

EVIDENCE
{f.get('evidence')}

WHAT IT SHOULD HAVE SAID
{f.get('expected')}

VERIFICATION
Qarlos re-checked this claim against the RAW catalogue record with an independent pass
instructed to refute it. Verdict: {verdict.get('verdict')}.
{verdict.get('reason','')}
{('Corrected evidence: ' + verdict['corrected_evidence']) if verdict.get('corrected_evidence') else ''}

REPRODUCE (deterministic — the seed pins the part selection)
  cd Kelvin && {repro}

The auditor drives web/src/crossref.js and the kelvin.js WASM directly, so this is
the same ranking path the site renders from, not a re-implementation of it.

RAW CASE
```json
{json.dumps(compact(case), ensure_ascii=False, indent=1)[:6000]}
```
"""
    title = f"Crossref: {f.get('title')}"[:160]
    if dry:
        print(f"    [dry-run] would file ABT: {title}")
        return None
    p = subprocess.run([ABT, "post", "--to", route, "--from", "qarlos",
                        "--title", title, "--body", body,
                        "--priority", SEV_TO_PRIORITY.get(f.get("severity"), "medium")],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print(f"    ABT post FAILED: {(p.stderr or p.stdout)[:200]}")
        return None
    m = re.search(r"Created issue #(\d+)", p.stdout or "")
    if not m:
        print(f"    ABT post gave no issue id: {(p.stdout or '')[:200]}")
        return None
    print(f"    filed ABT #{m.group(1)} -> {route}")
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-family", type=int, default=2)
    ap.add_argument("--max-results", type=int, default=8)
    ap.add_argument("--families", default="")
    ap.add_argument("--shards", default="")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--model", default="")
    # A judge call reads several full catalogue records; 300 s was not enough and the
    # timeout showed up as a lost case rather than a slow one.
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--file", action="store_true",
                    help="actually file ABT tickets (default: report only)")
    ap.add_argument("--json", default="",
                    help="write the run result here so a caller can re-test exactly")
    a = ap.parse_args()
    os.environ["QARLOS_PF"] = str(a.per_family)

    for tool, path in (("claude", CLAUDE), ("abt", ABT), ("node", NODE)):
        if not Path(path).exists() and not shutil.which(path):
            sys.exit(f"{tool} not found at {path} — set QARLOS_{tool.upper()}")

    cases, seed = run_probe(a)
    print(f"qarlos: {len(cases)} cases, seed {seed}, "
          f"{datetime.now(timezone.utc).isoformat(timespec='seconds')}")

    seen = load_seen()
    results = []      # machine-readable: what loop.py re-tests against
    n_ok = n_err = n_find = n_filed = n_dupe = n_refuted = 0
    for case in cases:
        if "error" in case:
            print(f"  {case['family']:10s} PROBE ERROR: {case['error'][:120]}")
            n_err += 1
            continue
        o = case["original"]
        label = f"{case['family']:10s} {o.get('mpn')} ({o.get('manufacturer')})"
        if not case.get("ranked"):
            print(f"  {label}: no candidates returned — nothing to audit")
            continue
        v = judge(case, a.model, a.timeout)
        if "_judge_error" in v:
            print(f"  {label}: JUDGE ERROR {v['_judge_error'][:160]}")
            n_err += 1
            continue
        if v.get("ok"):
            print(f"  {label}: ok ({len(case['ranked'])} candidates)")
            n_ok += 1
            continue
        for f in v.get("findings", []):
            sig = signature(case, f)
            n_find += 1
            sev = f.get("severity", "medium")
            print(f"  {label}: [{sev}] {f.get('title')}")
            print(f"    candidate {f.get('candidate')} | {f.get('evidence','')[:160]}")
            if sig in seen:
                print("    (already reported — not filing again)")
                n_dupe += 1
                continue
            # Scope it before judging worth: "one bad record" and "1,691 bad records"
            # are different tickets, and the second is the one that matters.
            f["_population"] = population_of(f)
            if f["_population"] and f["_population"].get("population") is not None:
                print(f"    population: {f['_population']['population']} of "
                      f"{f['_population']['scanned']} in {f['_population']['file']}")
            # Qarlos checks its own work: an independent pass, told to refute the
            # claim against the RAW record, decides whether it is worth a human's time.
            vv = verify(case, f, a.model, a.timeout)
            if vv.get("verdict") != "confirmed":
                print(f"    REFUTED by self-check: {vv.get('reason','')[:200]}")
                n_refuted += 1
                continue
            # The verifier may correct the routing and severity; trust the second look.
            f["defect_in"] = vv.get("defect_in", f.get("defect_in", "ranking"))
            f["severity"] = vv.get("severity", sev)
            print(f"    confirmed -> {DEFECT_ROUTE.get(f['defect_in'], 'kelvin')} "
                  f"[{f['severity']}] ({f['defect_in']})")
            tid = file_abt(case, f, seed, dry=not a.file, verdict=vv)
            results.append({
                "abt": tid, "signature": sig, "seed": seed,
                "family": case["family"], "original": o.get("mpn"),
                "candidate": f.get("candidate"), "title": f.get("title"),
                "severity": f.get("severity"), "defect_in": f.get("defect_in"),
                "repro": {"seed": seed, "families": case["family"],
                          "per_family": a.per_family},
                # loop.py re-runs this after the fix. A catalogue ticket is not closed
                # by the sampled part going clean — the population must go to zero.
                "population_query": f.get("population_query"),
                "population_before": (f.get("_population") or {}).get("population"),
            })
            if tid:
                seen.add(sig)
                n_filed += 1
    if a.file:
        save_seen(seen)
    if a.json:
        Path(a.json).write_text(json.dumps(
            {"seed": seed, "families": a.families, "per_family": a.per_family,
             "clean": n_ok, "claimed": n_find, "refuted": n_refuted,
             "errors": n_err, "findings": results}, ensure_ascii=False, indent=1))
    print(f"\nclean {n_ok} | claimed {n_find} | refuted by self-check {n_refuted} | "
          f"filed {n_filed} | duplicates {n_dupe} | errors {n_err}")
    print(f"reproduce this exact run:  qarlos.py --seed {seed} "
          f"--per-family {a.per_family}" + (f" --families {a.families}" if a.families else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
