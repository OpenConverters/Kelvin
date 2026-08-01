#!/usr/bin/env python3
"""Build the PUBLIC variant of a TAS catalog for kelvin.openconverters.com (ABT #456).

The master TAS data keeps full attribution. The publicly served copy must not
redistribute vendor marketing prose or index the exact source endpoints:

  stripped   manufacturerInfo.description            (verbatim vendor prose)
  stripped   ...datasheetInfo.part.description       (verbatim vendor prose)
  stripped   ...part.matchcodeDescription            (vendor matchcode text)
  coarsened  provenance[]: sourceName + sourceUrl removed — the coarse kind
             stays in provenance[].source, retrievedDate/fields/derivation stay.

  kept       manufacturerInfo.datasheetUrl (a link TO the vendor is attribution,
             not redistribution), all parametric facts, all derived data.

Works on any PEAS-wrapped family (walks the doc; a 'part' is a dict carrying
partNumber, a 'manufacturerInfo' is a dict carrying name+datasheetInfo).
Untouched lines are still re-serialized ONLY when modified — unmodified lines
are copied byte-identical so diffs stay reviewable.

Usage: strip-public-fields.py SRC.ndjson DST.ndjson
"""
import json
import sys


def strip(doc):
    changed = False

    def walk(node):
        nonlocal changed
        if isinstance(node, dict):
            # manufacturerInfo-shaped: name + datasheetInfo
            if "datasheetInfo" in node and "name" in node:
                if node.pop("description", None) is not None:
                    changed = True
            # part-shaped: partNumber
            if "partNumber" in node:
                if node.pop("description", None) is not None:
                    changed = True
                if node.pop("matchcodeDescription", None) is not None:
                    changed = True
            prov = node.get("provenance")
            if isinstance(prov, list):
                for entry in prov:
                    if isinstance(entry, dict):
                        if entry.pop("sourceName", None) is not None:
                            changed = True
                        if entry.pop("sourceUrl", None) is not None:
                            changed = True
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(doc)
    return changed


def main():
    src, dst = sys.argv[1], sys.argv[2]
    n = modified = 0
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        for raw in fin:
            n += 1
            try:
                doc = json.loads(raw)
            except Exception:
                fout.write(raw)
                continue
            if strip(doc):
                modified += 1
                fout.write((json.dumps(doc, ensure_ascii=False) + "\n").encode())
            else:
                fout.write(raw)
    print(f"{src}: {n} records, {modified} stripped -> {dst}")


if __name__ == "__main__":
    main()
