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


def write_family(S, out_dir: Path, family: str) -> int:
    """Write one family's sitemap files (replacing its old ones); returns its URL count.

    One family at a time so a deploy host can build under a small memory cap: a full catalogue
    in one process peaks near 2 GB (every shard resident), one family a fraction of that.
    """
    base = "https://" + (S._public_host or "kelvin.openconverters.com")
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob(f"sitemap-{family}-*.xml"):
        old.unlink()
    rows = list(family_rows(S, family))
    same_mpn = Counter(m.lower() for m, _ in rows)
    seen: set[tuple[str, str]] = set()
    urls = []
    for mpn, maker in rows:
        if (mpn, maker) in seen:                     # the same part twice is one URL
            continue
        seen.add((mpn, maker))
        urls.append(part_url(base, family, mpn, maker, same_mpn[mpn.lower()] > 1))
    for n, start in enumerate(range(0, len(urls), URLS_PER_FILE), 1):
        body = "".join(f"<url><loc>{escape(u)}</loc></url>\n"
                       for u in urls[start:start + URLS_PER_FILE])
        (out_dir / f"sitemap-{family}-{n}.xml").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n' + body + "</urlset>\n",
            encoding="utf-8")
    (out_dir / f"family-{family}.json").write_text(
        json.dumps({"urls": len(urls), "dataVersion": S._data_version()}), encoding="utf-8")
    return len(urls)


def finish(S, out_dir: Path, families: list[str] | None = None) -> dict:
    """Write the index and build.json from the family files present; every family must have
    been built from the data version in force now."""
    base = "https://" + (S._public_host or "kelvin.openconverters.com")
    version = S._data_version()
    counts = {}
    for family in families or S.kelvin.family_names():
        marker = out_dir / f"family-{family}.json"
        if not marker.is_file():
            raise ValueError(f"no sitemap built for {family}")
        built = json.loads(marker.read_text())
        if built["dataVersion"] != version:
            raise ValueError(f"{family} sitemap was built from another catalogue")
        counts[family] = built["urls"]
    files = sorted(p.name for p in out_dir.glob("sitemap-*-*.xml")
                   if p.name.split("-")[1] in counts)
    (out_dir / "sitemap.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<sitemapindex xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
        + "".join(f"<sitemap><loc>{base}/{f}</loc></sitemap>\n" for f in files)
        + "</sitemapindex>\n", encoding="utf-8")
    (out_dir / "build.json").write_text(json.dumps(
        {"dataVersion": version, "urls": counts, "files": len(files)}), encoding="utf-8")
    return counts


def build(S, out_dir: Path, families: list[str] | None = None) -> dict:
    """Everything in one process (tests, small hosts). `families` limits it; the index then
    lists only those."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in list(out_dir.glob("sitemap*.xml")) + list(out_dir.glob("family-*.json")):
        old.unlink()
    names = families or S.kelvin.family_names()
    for family in names:
        write_family(S, out_dir, family)
    return finish(S, out_dir, names)


if __name__ == "__main__":
    import argparse

    sys.path.insert(0, str(Path(__file__).parent))
    import server as S

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out_dir", nargs="?", type=Path, default=default_dir())
    ap.add_argument("--family", help="build only this family's files (low memory)")
    ap.add_argument("--finish", action="store_true", help="write the index once every family is built")
    ap.add_argument("--list-families", action="store_true")
    args = ap.parse_args()
    if args.list_families:
        print("\n".join(S.kelvin.family_names()))
    elif args.family:
        print(f"{args.family}: {write_family(S, args.out_dir, args.family):,} URLs")
    elif args.finish:
        result = finish(S, args.out_dir)
        print(f"{sum(result.values()):,} URLs in {len(result)} families -> {args.out_dir}")
    else:
        result = build(S, args.out_dir)
        print(f"{sum(result.values()):,} URLs in {len(result)} families -> {args.out_dir}")
