"""Public read-only JSON API over the same engine the MCP tools use (ABT #1916).

    GET  /api/v1/families
    GET  /api/v1/part/{family}/{mpn}[?manufacturer=]
    GET  /api/v1/crossref/{family}/{mpn}[?manufacturer=&target_manufacturers=a,b&same_type=&max_results=]
    POST /api/v1/select/{family}     {"requirements": {...}, "options": {...}, "max_results": 12}
    GET  /api/v1/search/{family}[?filters=<json>&sort=<json>&limit=&offset=&with_facets=]
    GET  /api/v1/openapi.json

There is no second implementation: every route calls the MCP tool function of the same name and
returns that tool's structured payload, so a verdict here is the verdict the MCP and the site
give. The wrapper adds only what a URL consumer needs to cite it — `dataVersion` and a
`permalink` — and a closed error shape. Registered from server.py so it shares the engine, the
public host and the telemetry sinks.
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import time
from pathlib import Path

import anyio
from starlette.responses import JSONResponse

API_VERSION = "v1"
_PREFIX = f"/api/{API_VERSION}"

# Per-IP sliding window, in memory only: the address is never written anywhere (the usage log
# and telemetry deliberately carry no IP).
RATE_LIMIT = int(os.environ.get("KELVIN_API_RATE_LIMIT", "60"))      # requests ...
RATE_WINDOW = float(os.environ.get("KELVIN_API_RATE_WINDOW", "60"))  # ... per this many seconds
_hits: dict[str, list[float]] = {}
_hits_lock = threading.Lock()


def _client(request) -> str:
    # Behind nginx the peer is always loopback, so the real client is the last hop it appended.
    forwarded = request.headers.get("x-forwarded-for", "")
    if forwarded:
        return forwarded.split(",")[-1].strip()
    return request.client.host if request.client else "unknown"


def _limited(request) -> float | None:
    """Seconds until the caller may retry, or None when the request is within the limit."""
    if RATE_LIMIT <= 0:
        return None
    now = time.monotonic()
    who = _client(request)
    with _hits_lock:
        window = [t for t in _hits.get(who, ()) if now - t < RATE_WINDOW]
        if len(window) >= RATE_LIMIT:
            _hits[who] = window
            return max(1.0, RATE_WINDOW - (now - window[0]))
        window.append(now)
        _hits[who] = window
        if len(_hits) > 10_000:                      # forget callers whose window has emptied
            for k in [k for k, v in _hits.items() if not v or now - v[-1] >= RATE_WINDOW]:
                del _hits[k]
    return None


def register(mcp, S) -> None:
    """Add the /api/v1 routes to `mcp`. `S` is the server module (engine, tools, helpers)."""

    base = lambda: "https://" + (S._public_host or "kelvin.openconverters.com")  # noqa: E731
    version_cache: dict[str, str] = {}

    def data_version() -> str:
        """Identifies the catalogue an answer was computed from.

        sha256 over each family file's name, size and mtime (symlinks followed) plus the shard
        manifest — it changes whenever a data deploy changes any family file. Computed once per
        process: the catalogue is replaced by a restart, not under a running server.
        """
        if "v" not in version_cache:
            h = hashlib.sha256()
            data = Path(S._data_dir())
            for f in sorted(data.glob("*.ndjson")):
                st = f.stat()
                h.update(f"{f.name}:{st.st_size}:{int(st.st_mtime)}\n".encode())
            shard_dir = (os.environ.get("KELVIN_SHARD_DIR", "").strip()
                         or str(S._REPO / "web" / "public" / "kelvin"))
            try:
                h.update((Path(shard_dir) / "manifest.json").read_bytes())
            except OSError:
                pass                                  # no manifest: the family files still count
            version_cache["v"] = h.hexdigest()[:16]
        return version_cache["v"]

    def permalink(family: str, mpn: str, manufacturer: str | None) -> str:
        from urllib.parse import quote
        url = f"{base()}/part/{quote(family)}/{quote(mpn, safe='')}"
        return url + (f"?manufacturer={quote(manufacturer, safe='')}" if manufacturer else "")

    def cite(obj):
        """Give every part object in a payload its permalink (in place)."""
        if isinstance(obj, dict):
            if obj.get("mpn") and obj.get("_family"):
                obj["permalink"] = permalink(obj.pop("_family"), obj["mpn"], obj.get("manufacturer"))
            for v in obj.values():
                cite(v)
        elif isinstance(obj, list):
            for v in obj:
                cite(v)

    def error(status: int, code: str, message: str, headers: dict | None = None):
        return JSONResponse({"error": {"code": code, "message": message}}, status_code=status,
                            headers=headers)

    async def run(request, route: str, family: str, call):
        """Rate-limit, run `call` off the event loop, wrap the payload."""
        wait = _limited(request)
        if wait is not None:
            return error(429, "rate_limited",
                         f"more than {RATE_LIMIT} requests per {RATE_WINDOW:g} s from this address",
                         {"Retry-After": str(int(wait + 0.999))})
        started = time.monotonic()
        try:
            result = await anyio.to_thread.run_sync(call)
        except S.kelvin.InvalidOptions as exc:        # the engine's own verdict on a bad request body
            S._telemetry("api_" + route, family, {"channel": "api", "ok": False}, _PREFIX + "/" + route)
            return error(400, "bad_request", str(exc))
        except ValueError as exc:                     # unknown family/field, no match, bad input
            S._telemetry("api_" + route, family, {"channel": "api", "ok": False}, _PREFIX + "/" + route)
            message = str(exc)
            return error(404 if message.startswith("no ") else 400,
                         "not_found" if message.startswith("no ") else "bad_request", message)
        payload = dict(result.structuredContent or {})
        payload.setdefault("family", family)
        S._telemetry("api_" + route, payload.get("family"),
                     {"channel": "api", "ok": True, "seconds": round(time.monotonic() - started, 3)},
                     _PREFIX + "/" + route)
        return JSONResponse({"dataVersion": data_version(), **payload},
                            headers={"Cache-Control": "public, max-age=300"})

    def tag_family(payload_call, family):
        """Run a tool and mark each candidate/part with the family so cite() can link it."""
        def call():
            result = payload_call()
            key = S._family(family)

            def mark(o):
                if isinstance(o, dict):
                    if o.get("mpn"):
                        o["_family"] = key
                    for v in o.values():
                        mark(v)
                elif isinstance(o, list):
                    for v in o:
                        mark(v)
            sc = result.structuredContent
            if sc is not None:
                mark(sc)
                cite(sc)
            return result
        return call

    @mcp.custom_route(f"{_PREFIX}/families", methods=["GET"])
    async def api_families(request):
        wait = _limited(request)
        if wait is not None:
            return error(429, "rate_limited", "rate limit", {"Retry-After": str(int(wait + 1))})
        return JSONResponse({"dataVersion": data_version(), "families": S.kelvin.family_names()},
                            headers={"Cache-Control": "public, max-age=300"})

    @mcp.custom_route(_PREFIX + "/part/{family}/{mpn:path}", methods=["GET"])
    async def api_part(request):
        q = request.query_params
        fam, mpn = request.path_params["family"], request.path_params["mpn"]
        return await run(request, "part", fam, tag_family(
            lambda: S.part_details(fam, mpn, q.get("manufacturer"), True), fam))

    @mcp.custom_route(_PREFIX + "/crossref/{family}/{mpn:path}", methods=["GET"])
    async def api_crossref(request):
        q = request.query_params
        fam, mpn = request.path_params["family"], request.path_params["mpn"]
        targets = [t for t in (q.get("target_manufacturers") or "").split(",") if t.strip()]
        same_type = (q.get("same_type", "true").lower() not in ("0", "false", "no"))
        try:
            limit = int(q.get("max_results", "12"))
        except ValueError:
            return error(400, "bad_request", "max_results must be an integer")
        return await run(request, "crossref", fam, tag_family(
            lambda: S.cross_reference(fam, mpn, q.get("manufacturer"), targets or None,
                                      same_type, limit), fam))

    @mcp.custom_route(_PREFIX + "/select/{family}", methods=["POST"])
    async def api_select(request):
        fam = request.path_params["family"]
        try:
            body = await request.json()
        except ValueError:
            return error(400, "bad_request", "the body must be JSON")
        if not isinstance(body, dict) or not isinstance(body.get("requirements"), dict):
            return error(400, "bad_request", "the body must be {\"requirements\": {...}}")
        try:
            limit = int(body.get("max_results", 12))
        except (TypeError, ValueError):
            return error(400, "bad_request", "max_results must be an integer")
        return await run(request, "select", fam, tag_family(
            lambda: S.recommend_parts(fam, body["requirements"], body.get("options"), limit,
                                      False), fam))

    @mcp.custom_route(_PREFIX + "/search/{family}", methods=["GET"])
    async def api_search(request):
        q = request.query_params
        fam = request.path_params["family"]
        try:
            filters = json.loads(q["filters"]) if q.get("filters") else None
            sort = json.loads(q["sort"]) if q.get("sort") else None
            limit, offset = int(q.get("limit", "25")), int(q.get("offset", "0"))
        except ValueError:
            return error(400, "bad_request", "filters and sort must be JSON; limit and offset integers")
        facets = q.get("with_facets", "false").lower() in ("1", "true", "yes")
        return await run(request, "search", fam, tag_family(
            lambda: S.search_parts(fam, filters, sort, limit, offset, facets), fam))

    @mcp.custom_route(f"{_PREFIX}/openapi.json", methods=["GET"])
    async def api_openapi(_request):
        return JSONResponse(openapi(base()), headers={"Cache-Control": "public, max-age=3600"})


def openapi(server_url: str) -> dict:
    """The OpenAPI 3.1 description of the routes above."""
    family = {"name": "family", "in": "path", "required": True, "schema": {"type": "string"},
              "description": "e.g. capacitor, mosfet, magnetic — see /families"}
    mpn = {"name": "mpn", "in": "path", "required": True, "schema": {"type": "string"}}
    maker = {"name": "manufacturer", "in": "query", "schema": {"type": "string"},
             "description": "needed only when two vendors ship the same MPN"}
    ok = {"description": "The tool payload (same DTO as the MCP tool) plus dataVersion",
          "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Result"}}}}
    err = {"description": "Error", "content": {"application/json": {
        "schema": {"$ref": "#/components/schemas/Error"}}}}
    responses = {"200": ok, "400": err, "404": err, "429": err}
    return {
        "openapi": "3.1.0",
        "info": {"title": "Kelvin API", "version": API_VERSION,
                 "description": "Read-only, deterministic power-electronics parts catalogue. "
                                "Values are SI base units; an absent spec is unstated, never 0. "
                                "Every answer carries dataVersion and each part a permalink.",
                 "license": {"name": "CC BY 4.0 (data)", "identifier": "CC-BY-4.0"}},
        "servers": [{"url": server_url}],
        "paths": {
            f"{_PREFIX}/families": {"get": {"operationId": "listFamilies", "responses": {"200": ok}}},
            f"{_PREFIX}/part/{{family}}/{{mpn}}": {"get": {
                "operationId": "getPart", "parameters": [family, mpn, maker], "responses": responses}},
            f"{_PREFIX}/crossref/{{family}}/{{mpn}}": {"get": {
                "operationId": "crossReference",
                "parameters": [family, mpn, maker,
                               {"name": "target_manufacturers", "in": "query",
                                "schema": {"type": "string"}, "description": "comma-separated"},
                               {"name": "same_type", "in": "query", "schema": {"type": "boolean", "default": True}},
                               {"name": "max_results", "in": "query",
                                "schema": {"type": "integer", "default": 12, "maximum": 40}}],
                "responses": responses}},
            f"{_PREFIX}/select/{{family}}": {"post": {
                "operationId": "selectParts", "parameters": [family],
                "requestBody": {"required": True, "content": {"application/json": {"schema": {
                    "type": "object", "required": ["requirements"],
                    "properties": {"requirements": {"type": "object"}, "options": {"type": "object"},
                                   "max_results": {"type": "integer", "default": 12, "maximum": 50}}}}}},
                "responses": responses}},
            f"{_PREFIX}/search/{{family}}": {"get": {
                "operationId": "searchParts",
                "parameters": [family,
                               {"name": "filters", "in": "query", "schema": {"type": "string"},
                                "description": "JSON object, as search_parts"},
                               {"name": "sort", "in": "query", "schema": {"type": "string"}},
                               {"name": "limit", "in": "query", "schema": {"type": "integer", "default": 25}},
                               {"name": "offset", "in": "query", "schema": {"type": "integer", "default": 0}},
                               {"name": "with_facets", "in": "query", "schema": {"type": "boolean"}}],
                "responses": responses}},
        },
        "components": {"schemas": {
            "Result": {"type": "object", "required": ["dataVersion"],
                       "properties": {"dataVersion": {"type": "string"},
                                      "mode": {"enum": ["details", "search", "recommend", "crossref"]},
                                      "family": {"type": "string"}},
                       "additionalProperties": True},
            "Error": {"type": "object", "required": ["error"], "properties": {"error": {
                "type": "object", "required": ["code", "message"],
                "properties": {"code": {"type": "string"}, "message": {"type": "string"}}}}},
        }},
    }
