"""Sitemaps and family index pages (ABT #1915).

    KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 -m pytest mcp/test_sitemaps.py

Every URL a sitemap lists must resolve to a part page — a sitemap that lists parts the site
cannot serve sends crawlers to 404s — so the test fetches a sample of them through the app.
"""

import re
import sys
from pathlib import Path
from urllib.parse import urlsplit
from xml.etree import ElementTree

sys.path.insert(0, str(Path(__file__).parent))

import pytest
import server as S
import sitemaps
from starlette.testclient import TestClient

NS = {"s": "http://www.sitemaps.org/schemas/sitemap/0.9"}


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    out = tmp_path_factory.mktemp("sitemaps")
    counts = sitemaps.build(S, out, families=["igbt", "bjt"])
    return out, counts


@pytest.fixture()
def client(built, monkeypatch):
    monkeypatch.setenv("KELVIN_SITEMAP_DIR", str(built[0]))
    return TestClient(S.build_app(), raise_server_exceptions=False)


def locs(xml: str) -> list[str]:
    return [e.text for e in ElementTree.fromstring(xml).iterfind(".//s:loc", NS)]


def test_index_lists_files_and_every_listed_part_resolves(built, client):
    index = client.get("/sitemap.xml")
    assert index.status_code == 200 and index.headers["content-type"].startswith("application/xml")
    files = locs(index.text)
    assert files and all(re.search(r"/sitemap-(igbt|bjt)-\d+\.xml$", f) for f in files)
    urls = []
    for f in files:
        child = client.get(urlsplit(f).path)
        assert child.status_code == 200
        urls += locs(child.text)
    assert len(urls) == sum(built[1].values()) and len(set(urls)) == len(urls)
    for url in urls[:: max(1, len(urls) // 25)]:         # a spread over both families
        parts = urlsplit(url)
        response = client.get(parts.path + ("?" + parts.query if parts.query else ""))
        assert response.status_code == 200, url


def test_a_catalogue_change_makes_the_sitemap_refuse_to_serve(built, client, monkeypatch):
    import json
    build_file = built[0] / "build.json"
    good = build_file.read_text()
    try:
        build_file.write_text(json.dumps({**json.loads(good), "dataVersion": "0" * 16}))
        assert client.get("/sitemap.xml").status_code == 503
    finally:
        build_file.write_text(good)
    assert client.get("/sitemap.xml").status_code == 200


def test_no_path_tricks_and_unknown_files_404(client):
    assert client.get("/sitemap-nosuch-9.xml").status_code == 404
    assert client.get("/sitemap-..%2Fbuild.json.xml").status_code == 404


def test_family_index_pages_page_through_the_family(client):
    first = client.get("/part/igbt")
    assert first.status_code == 200 and "page 1 of" in first.text and "/part/igbt/" in first.text
    assert client.get("/part/igbt?page=99999").status_code == 404
    assert client.get("/part/igbt?page=x").status_code == 400
    assert client.get("/part/nosuchfamily").status_code == 404
