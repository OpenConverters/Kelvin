#!/usr/bin/env python3
"""A crawler's-eye check of the live part pages (ABT #1915) — what a search engine or an AI
fetcher that does not run JavaScript receives. Stands in for the Search Console indexing sample.

    python3 mcp/check_crawl.py [BASE_URL] [SAMPLE]

robots.txt must allow the pages and name the sitemap; the sitemap index and its children must be
well-formed; then SAMPLE pages spread over the children are fetched with no JS and must carry the
MPN in <title> and <h1>, a parseable schema.org Product (JSON-LD) naming the same MPN, a spec
table, a canonical JSON alternate, and the substitutes section. Paced under 40 requests/minute.
"""

import json
import re
import sys
import time
import urllib.error
import urllib.request
from html import unescape
from xml.etree import ElementTree

BASE = (sys.argv[1] if len(sys.argv) > 1 else "https://kelvin.openconverters.com").rstrip("/")
SAMPLE = int(sys.argv[2]) if len(sys.argv) > 2 else 40
NS = {"s": "http://www.sitemaps.org/schemas/sitemap/0.9"}


def get(url: str) -> tuple[int, str]:
    for _ in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(
                    url, headers={"User-Agent": "kelvin-crawl-check"}), timeout=120) as r:
                code, body = r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            code, body = e.code, ""
        if code != 429:
            time.sleep(1.6)
            return code, body
        time.sleep(8)
    return 429, ""


def main() -> int:
    problems = []
    code, robots = get(BASE + "/robots.txt")
    if code != 200 or "Sitemap: " + BASE + "/sitemap.xml" not in robots or "Disallow: /" in robots.replace("Disallow: /\n", "\0"):
        problems.append(f"robots.txt: {code}, must allow crawling and name {BASE}/sitemap.xml")
    code, index = get(BASE + "/sitemap.xml")
    children = [e.text for e in ElementTree.fromstring(index).iterfind(".//s:loc", NS)] if code == 200 else []
    if not children:
        problems.append(f"sitemap.xml: {code}, no children")
    per = max(1, SAMPLE // max(1, len(children)))
    urls = []
    for child in children:
        code, xml = get(child)
        locs = [e.text for e in ElementTree.fromstring(xml).iterfind(".//s:loc", NS)] if code == 200 else []
        if not locs:
            problems.append(f"{child}: {code}, no URLs")
        urls += locs[:: max(1, len(locs) // per)][:per]
    checked = 0
    for url in urls[:SAMPLE]:
        code, page = get(url)
        checked += 1
        if code != 200:
            problems.append(f"{url}: {code}")
            continue
        mpn = unescape(re.search(r"<h1>(.*?)</h1>", page, re.S).group(1)) if "<h1>" in page else ""
        ld = re.search(r'<script type="application/ld\+json">(.*?)</script>', page, re.S)
        checks = {
            "h1 holds the MPN": bool(mpn),
            "title holds the MPN": bool(mpn) and mpn in unescape(re.search(r"<title>(.*?)</title>", page, re.S).group(1)),
            "JSON-LD Product names the same MPN": bool(ld) and json.loads(ld.group(1)).get("mpn") == mpn,
            "spec table": "<table>" in page and "<th>" in page,
            "JSON alternate link": "rel=alternate type='application/json'" in page,
            "substitutes section": "<h2>Substitutes</h2>" in page,
        }
        for name, ok in checks.items():
            if not ok:
                problems.append(f"{url}: missing — {name}")
    print(f"robots.txt, {len(children)} sitemap files, {checked} pages fetched without JavaScript")
    for p in problems:
        print("PROBLEM", p)
    print(f"{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
