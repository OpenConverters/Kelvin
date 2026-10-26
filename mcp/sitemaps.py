"""sitemap.xml for the per-part pages (ABT #1915).

    KELVIN_TAS_DATA_DIR=... python3 mcp/sitemaps.py [OUT_DIR]     # default ~/.kelvin/sitemaps

Writes one sitemap index plus sitemap-<family>-<n>.xml files of at most URLS_PER_FILE part URLs
(the protocol allows 50,000), enumerating the SAME rows the part page can serve — the engine's
shard, not the raw NDJSON — so every URL listed resolves. server.py serves the directory at
/sitemap.xml and /sitemap-*.xml. Rebuilt on every data deploy; the index records the data
version it was built from, and the server refuses to serve a directory built from another one.
"""

from __future__ import annotations

import json
import os
import sys
from collections import Counter
from pathlib import Path
from urllib.parse import quote
from xml.sax.saxutils import escape

URLS_PER_FILE = 40_000
PAGE = 1000                         # the engine's browse() maximum


def default_dir() -> Path:
    return Path(os.environ.get("KELVIN_SITEMAP_DIR", "").strip() or Path.home() / ".kelvin/sitemaps")


def family_rows(S, family: str):
    """(mpn, manufacturer) for every row of one family, in shard order."""
    offset = 0
    while True:
        page = S._browse(family, {"limit": PAGE, "offset": offset})
        rows = page.get("rows") or []
        for r in rows:
            if r.get("mpn"):
                yield r["mpn"], r.get("manufacturer") or ""
        offset += len(rows)
        if len(rows) < PAGE or offset >= page.get("total", offset):
            return


def part_url(base: str, family: str, mpn: str, manufacturer: str, ambiguous: bool) -> str:
    url = f"{base}/part/{quote(family)}/{quote(mpn, safe='')}"
    return url + (f"?manufacturer={quote(manufacturer, safe='')}" if ambiguous and manufacturer else "")


def build(S, out_dir: Path, families: list[str] | None = None) -> dict:
    """Write the sitemaps; returns {family: urls}. `S` is the server module."""
    base = "https://" + (S._public_host or "kelvin.openconverters.com")
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("sitemap*.xml"):
        old.unlink()
    counts: dict[str, int] = {}
    files: list[str] = []
    for family in families or S.kelvin.family_names():
        rows = list(family_rows(S, family))
        same_mpn = Counter(m.lower() for m, _ in rows)
        seen: set[tuple[str, str]] = set()
        urls = []
        for mpn, maker in rows:
            if (mpn, maker) in seen:                 # the same part twice is one URL
                continue
            seen.add((mpn, maker))
            urls.append(part_url(base, family, mpn, maker, same_mpn[mpn.lower()] > 1))
        counts[family] = len(urls)
        for n, start in enumerate(range(0, len(urls), URLS_PER_FILE), 1):
            name = f"sitemap-{family}-{n}.xml"
            body = "".join(f"<url><loc>{escape(u)}</loc></url>\n"
                           for u in urls[start:start + URLS_PER_FILE])
            (out_dir / name).write_text(
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n' + body + "</urlset>\n",
                encoding="utf-8")
            files.append(name)
    (out_dir / "sitemap.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<sitemapindex xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
        + "".join(f"<sitemap><loc>{base}/{f}</loc></sitemap>\n" for f in files)
        + "</sitemapindex>\n", encoding="utf-8")
    (out_dir / "build.json").write_text(json.dumps(
        {"dataVersion": S._data_version(), "urls": counts, "files": len(files)}), encoding="utf-8")
    return counts


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).parent))
    import server as S

    target = Path(sys.argv[1]) if len(sys.argv) > 1 else default_dir()
    result = build(S, target)
    print(f"{sum(result.values()):,} URLs in {len(result)} families -> {target}")
    for family, n in result.items():
        print(f"  {family:11s}{n:>10,}")
