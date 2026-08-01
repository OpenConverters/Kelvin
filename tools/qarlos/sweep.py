#!/usr/bin/env python3
"""How many records share this defect — the difference between a fix and a patch.

Qarlos finds a defect on ONE part, because that is how sampling works. But a units
error in a catalogue is almost never one record: it is whatever the importer wrote that
day. If the loop treats "the sampled part is clean now" as fixed, Minion repairs one row,
the re-test passes, the ticket resolves, and the other several thousand stay broken and
un-ticketed — which is worse than never having looked, because the board now says it was
handled.

So a catalogue finding carries a POPULATION QUERY: a declarative predicate describing
every record with the same defect. It is used three times —
  * when filing, so the ticket says "this affects N records", not "this part is wrong";
  * by Minion, which must drive N to zero (repair or quarantine, never delete);
  * by the re-test, which fails the fix unless N is zero.

Code defects (extraction/ranking) need none of this: one extractor change fixes every
part in the family by construction.

THE PREDICATE IS DECLARATIVE ON PURPOSE. It would be easier to let the judge emit a
Python expression and eval it. That runs model-authored code over the catalogue with the
loop's own permissions, on a box that also holds the deploy keys, and a subtly wrong
expression could match everything. A fixed grammar of path/op/value cannot do anything
except read fields and compare them.

  sweep.py --query '{"file":"capacitors.ndjson","all":[
      {"path":"capacitor.manufacturerInfo.datasheetInfo.electrical.esr","op":"<","value":0.01},
      {"path":"capacitor.manufacturerInfo.datasheetInfo.part.technology","op":"startswith","value":"aluminum"}]}'
  sweep.py --query-file q.json --limit 20
"""
import argparse
import json
import sys
from pathlib import Path

TAS = Path.home() / "PSMA" / "TAS" / "data"

OPS = {
    "<": lambda a, b: _num(a) is not None and _num(a) < b,
    "<=": lambda a, b: _num(a) is not None and _num(a) <= b,
    ">": lambda a, b: _num(a) is not None and _num(a) > b,
    ">=": lambda a, b: _num(a) is not None and _num(a) >= b,
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
    "exists": lambda a, b: (a is not None) == bool(b),
    "startswith": lambda a, b: isinstance(a, str) and a.startswith(b),
    "contains": lambda a, b: isinstance(a, str) and b in a,
    "in": lambda a, b: a in b,
}


def _num(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def dig(obj, path):
    """Dotted path with [] meaning 'any element of this array matches downstream'."""
    cur = obj
    for i, part in enumerate(path.split(".")):
        if part == "[]":
            rest = ".".join(path.split(".")[i + 1:])
            if not isinstance(cur, list):
                return None
            for el in cur:
                got = dig(el, rest) if rest else el
                if got is not None:
                    return got
            return None
        if not isinstance(cur, dict):
            return None
        cur = cur.get(part)
        if cur is None:
            return None
    return cur


def match(rec, conds):
    for c in conds:
        val = dig(rec, c["path"])
        op = OPS.get(c.get("op", "=="))
        if op is None:
            raise ValueError(f"unknown op {c.get('op')!r}")
        if not op(val, c.get("value")):
            return False
    return True


def run(query, limit=10, data_dir=TAS):
    path = Path(data_dir) / query["file"]
    if not path.exists():
        raise SystemExit(f"no such catalogue file: {path}")
    conds = query.get("all") or []
    if not conds:
        raise SystemExit("query has no conditions — refusing to match every record")
    hits, n, samples = 0, 0, []
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            n += 1
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if match(rec, conds):
                hits += 1
                if len(samples) < limit:
                    samples.append(_ref(rec))
    return {"file": query["file"], "scanned": n, "population": hits,
            "samples": samples}


def _ref(rec):
    """'<vendor>/<part number>' for a sample line. Not every catalogue fills
    manufacturerInfo.reference, so fall back to the datasheet part number rather than
    printing a list of 'Vendor/None' that identifies nothing."""
    for _ in range(3):
        if isinstance(rec, dict) and "manufacturerInfo" in rec:
            mi = rec["manufacturerInfo"]
            pn = (mi.get("reference")
                  or dig(mi, "datasheetInfo.part.partNumber")
                  or mi.get("orderCode") or "?")
            return f"{mi.get('name')}/{pn}"
        if isinstance(rec, dict) and rec:
            rec = next(iter(rec.values()))
        else:
            break
    return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="")
    ap.add_argument("--query-file", default="")
    ap.add_argument("--limit", type=int, default=10)
    ap.add_argument("--data", default=str(TAS))
    a = ap.parse_args()
    if not (a.query or a.query_file):
        sys.exit("need --query or --query-file")
    q = json.loads(Path(a.query_file).read_text() if a.query_file else a.query)
    out = run(q, a.limit, a.data)
    print(json.dumps(out, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
