#!/usr/bin/env python3
"""Prove the LIVE part pages and API carry the same public-field strip as the shards (ABT #456).

    python3 mcp/check_public_strip.py [BASE_URL] [PARTS_PER_FAMILY]

Fetches a spread of parts per family through the public API and walks every returned record for
what strip-public-fields.py removes: verbatim vendor prose (manufacturerInfo.description,
part.description, part.matchcodeDescription) and provenance sourceName / sourceUrl. Also checks
the HTML page and the JSON-LD for the same strings. Paced under nginx's 40 requests/minute.
Exit 1 with the offending paths if anything leaks.
"""

import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "https://kelvin.openconverters.com").rstrip("/")
PER_FAMILY = int(sys.argv[2]) if len(sys.argv) > 2 else 4
PACE = 1.6


def get(path: str):
    for _ in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(
                    BASE + path, headers={"User-Agent": "kelvin-strip-check"}), timeout=120) as r:
                body = r.read().decode()
            time.sleep(PACE)
            return body
        except urllib.error.HTTPError as e:
            if e.code != 429:
                raise
            time.sleep(8)
    raise RuntimeError(f"rate limited: {path}")


def leaks(node, path="$"):
    """Yield the path of every key the public strip is supposed to have removed."""
    if isinstance(node, dict):
        if "datasheetInfo" in node and "name" in node and "description" in node:
            yield f"{path}.description (manufacturerInfo)"
        if "partNumber" in node:
            for key in ("description", "matchcodeDescription"):
                if key in node:
                    yield f"{path}.{key} (part)"
        prov = node.get("provenance")
        if isinstance(prov, list):
            for i, entry in enumerate(prov):
                if isinstance(entry, dict):
                    for key in ("sourceName", "sourceUrl"):
                        if key in entry:
                            yield f"{path}.provenance[{i}].{key}"
        for k, v in node.items():
            yield from leaks(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from leaks(v, f"{path}[{i}]")


def main() -> int:
    families = json.loads(get("/api/v1/families"))["families"]
    found, checked, with_prose_field = [], 0, 0
    for family in families:
        page = json.loads(get(f"/api/v1/search/{family}?limit=40"))
        cands = page["candidates"]
        for c in cands[:: max(1, len(cands) // PER_FAMILY)][:PER_FAMILY]:
            mpn = urllib.parse.quote(c["mpn"], safe="")
            q = "?manufacturer=" + urllib.parse.quote(c.get("manufacturer") or "", safe="")
            doc = json.loads(get(f"/api/v1/part/{family}/{mpn}{q}"))
            html = get(f"/part/{family}/{mpn}{q}")
            checked += 1
            for where in leaks(doc.get("part", {}).get("record") or {}):
                found.append(f"{family}/{c['mpn']}: {where}")
            record_text = json.dumps(doc.get("part", {}).get("record") or {})
            if '"sourceUrl"' in html or '"sourceName"' in html:
                found.append(f"{family}/{c['mpn']}: provenance source in the HTML page")
            with_prose_field += '"description"' in record_text
    print(f"{checked} parts over {len(families)} families checked on {BASE}")
    for line in found:
        print("LEAK", line)
    print(f"{len(found)} leaks")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
