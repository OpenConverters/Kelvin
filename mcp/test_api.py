"""The public /api/v1 routes return exactly what the MCP tools return (ABT #1916).

    KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 -m pytest mcp/test_api.py

Parity is the point: the API has no engine of its own, so for the same inputs the payload must
equal the tool's structured result once the two things the API adds (dataVersion, permalink)
are taken away. Needs the real catalogue — it fails loudly without one, it does not skip.
"""

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import pytest
import server as S
from starlette.testclient import TestClient

SIX = "https://kelvin.openconverters.com/part/"


@pytest.fixture(scope="module")
def client():
    return TestClient(S.build_app(), raise_server_exceptions=False)


def strip(o):
    """Remove what the API adds, so the rest can be compared with the tool's payload."""
    if isinstance(o, dict):
        return {k: strip(v) for k, v in o.items()
                if k not in ("permalink", "sourceUrl", "dataVersion",
                             "_permalink", "_sourceUrl", "_dataVersion")}
    if isinstance(o, list):
        return [strip(v) for v in o]
    return o


def tool_payload(result):
    return json.loads(json.dumps(result.structuredContent))


def first_mpn(client):
    r = client.get("/api/v1/search/capacitor",
                   params={"filters": json.dumps({"manufacturer": ["Murata"]}), "limit": 1})
    assert r.status_code == 200
    return r.json()["candidates"][0]["mpn"]


def test_search_part_select_match_the_tools(client):
    mpn = first_mpn(client)
    cases = [
        (client.get("/api/v1/search/capacitor",
                    params={"filters": json.dumps({"manufacturer": ["Murata"]}), "limit": 3}),
         S.search_parts("capacitor", {"manufacturer": ["Murata"]}, None, 3, 0, False)),
        (client.get(f"/api/v1/part/capacitor/{mpn}"), S.part_details("capacitor", mpn, None, True)),
        (client.post("/api/v1/select/capacitor",
                     json={"requirements": {"capacitance": 1e-7, "ratedVoltage": 50}, "max_results": 3}),
         S.recommend_parts("capacitor", {"capacitance": 1e-7, "ratedVoltage": 50}, None, 3, False)),
    ]
    for response, result in cases:
        assert response.status_code == 200
        body = response.json()
        assert re.fullmatch(r"[0-9a-f]{16}", body["dataVersion"])
        assert strip(body) == strip(tool_payload(result))


def test_crossref_matches_the_tool(client):
    mpn = first_mpn(client)
    response = client.get(f"/api/v1/crossref/capacitor/{mpn}", params={"max_results": 3})
    assert response.status_code == 200
    assert strip(response.json()) == strip(tool_payload(
        S.cross_reference("capacitor", mpn, None, None, True, 3)))


def test_every_part_carries_its_permalink(client):
    body = client.get("/api/v1/search/capacitor", params={"limit": 3}).json()
    assert body["candidates"]
    for c in body["candidates"]:
        assert c["permalink"].startswith(SIX + "capacitor/"), c
    assert "_permalink" not in json.dumps(body)


def test_errors_have_one_closed_shape(client):
    for response, status in [
        (client.get("/api/v1/part/capacitor/NO-SUCH-PART-XYZ"), 404),
        (client.get("/api/v1/part/zzz/x"), 400),
        (client.post("/api/v1/select/capacitor", json={"requirements": {"capacitance": {"nominal": 1}}}), 400),
        (client.post("/api/v1/select/capacitor", content=b"not json"), 400),
        (client.get("/api/v1/search/capacitor", params={"filters": "{"}), 400),
    ]:
        assert response.status_code == status, response.text
        assert set(response.json()) == {"error"}
        assert set(response.json()["error"]) == {"code", "message"}


def test_rate_limit_answers_429_with_retry_after(client, monkeypatch):
    import api
    monkeypatch.setattr(api, "RATE_LIMIT", 2)
    api._hits.clear()
    responses = [client.get("/api/v1/families") for _ in range(3)]
    api._hits.clear()
    assert [r.status_code for r in responses] == [200, 200, 429]
    assert responses[2].headers["retry-after"].isdigit()
    assert responses[2].json()["error"]["code"] == "rate_limited"


def test_openapi_is_valid_and_covers_every_route(client):
    from openapi_spec_validator import validate
    spec = client.get("/api/v1/openapi.json").json()
    validate(spec)
    served = {r.path for r in S.mcp._custom_starlette_routes if r.path.startswith("/api/v1/")}
    documented = {re.sub(r"\{mpn\}", "{mpn:path}", p) for p in spec["paths"]} | {"/api/v1/openapi.json"}
    assert served == documented


def test_part_page_links_to_its_json_alternate(client):
    mpn = first_mpn(client)
    html = client.get(f"/part/capacitor/{mpn}").text
    href = re.search(r"<link rel=alternate type='application/json' href='([^']+)'", html).group(1)
    assert href.startswith("https://kelvin.openconverters.com/api/v1/part/capacitor/")
    local = href.split("kelvin.openconverters.com", 1)[1]
    assert client.get(local).status_code == 200


def test_non_finite_numbers_become_null_not_a_500(client, monkeypatch):
    """The engine reports an unstated value as NaN; JSON cannot carry it (found live: a select
    result with a NaN margin answered 500)."""
    from mcp.types import CallToolResult, TextContent
    nan = CallToolResult(content=[TextContent(type="text", text="x")],
                         structuredContent={"mode": "recommend", "family": "capacitor",
                                            "candidates": [{"mpn": "A", "margins": {"m": float("nan"),
                                                                                    "n": float("inf")}}]})
    monkeypatch.setattr(S, "recommend_parts", lambda *a, **k: nan)
    response = client.post("/api/v1/select/capacitor", json={"requirements": {}})
    assert response.status_code == 200
    assert response.json()["candidates"][0]["margins"] == {"m": None, "n": None}


def test_part_page_lists_substitutes_from_cache_then_survives_busy_worker(tmp_path, monkeypatch, client):
    monkeypatch.setattr(S, "PAGE_CACHE_DIR", str(tmp_path))
    monkeypatch.setattr(S, "_page_cache_written", 0)
    mpn = first_mpn(client)
    html = client.get(f"/part/capacitor/{mpn}").text
    assert "<h2>Substitutes</h2>" in html and "scored candidates" in html
    assert "<th>Grade</th>" in html and html.count("/part/capacitor/") >= 3
    cached = json.loads(next(tmp_path.rglob("*.json")).read_text())
    assert cached["candidates"] and all(c["manufacturer"] for c in cached["candidates"])
    assert list(tmp_path.rglob("*.json")), "the computed table was not cached"

    # A cached page needs no worker at all.
    monkeypatch.setattr(S, "_xref", lambda *_: (_ for _ in ()).throw(AssertionError("recomputed")))
    assert "<th>Grade</th>" in client.get(f"/part/capacitor/{mpn}").text

    # An uncached page asked for while the worker is busy renders without a table, not a wait.
    other = client.get("/api/v1/search/capacitor", params={"limit": 2}).json()["candidates"][1]["mpn"]
    assert S._page_xref_lock.acquire(blocking=False)
    try:
        busy = client.get(f"/part/capacitor/{other}")
    finally:
        S._page_xref_lock.release()
    assert busy.status_code == 200 and "Computed on request" in busy.text
