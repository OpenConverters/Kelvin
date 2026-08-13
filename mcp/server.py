"""Kelvin MCP server — the parts catalogue as a chat-reachable librarian.

Kelvin is the deterministic component selector over the TAS catalogue. Kirchhoff's
MCP server exposes a design-oriented slice of it (source parts FOR a converter);
this one exposes the catalogue itself, so the questions that do not need a design
to exist first — "what X2 caps do we stock between 100 nF and 470 nF", "what does
this competitor part cross-reference to", "how is Rds(on) distributed across 100 V
silicon" — can be asked directly.

Every answer comes from the same C++ engine the web app and Kirchhoff run:

    catalogue search / lookup / recommend   PyKelvin (native, in-process)
    cross-reference                         xref.mjs (the web app's own
                                            crossref.js over the same WASM build)

The split is deliberate. The substitute RANKER is C++ and PyKelvin exposes it, but
turning "this MPN" into a scored candidate list also needs the per-family pre-gate
and the shard-row -> spec projection, and those live in web/src/crossref.js — the
module Qarlos (the standing cross-reference auditor) drives. Re-writing that table
here would make the MCP surface a second copy of it, and a second copy drifts. See
xref.mjs.

Run:
    KELVIN_TAS_DATA_DIR=/path/to/catalogue python3 mcp/server.py   # 127.0.0.1:8402/mcp
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import threading
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
_SEARCHED = (_REPO / "build", _REPO / "build-latest")
_FOUND = [p for d in _SEARCHED for p in list(d.glob("PyKelvin*.so")) + list(d.glob("PyKelvin*.pyd"))]
for _module in _FOUND:
    sys.path.insert(0, str(_module.parent))
    break

try:
    import PyKelvin as kelvin
except ImportError as error:                                       # pragma: no cover
    # A built module the RUNNING interpreter cannot load is the common failure (the
    # build targeted a different python3), and "not built" would send you off
    # rebuilding something that is already there. Say which it is.
    _running = f"{sys.version_info.major}.{sys.version_info.minor}"
    if _FOUND:
        _tags = ", ".join(sorted({p.name for p in _FOUND}))
        raise ImportError(
            f"PyKelvin IS built ({_tags}) but this interpreter cannot load it: "
            f"you are running Python {_running} ({sys.executable}).\n"
            f"Run the server with the interpreter the module was built for, or rebuild:\n"
            f"  cmake -S . -B build -G Ninja -DPython3_EXECUTABLE={sys.executable} "
            f"&& ninja -C build PyKelvin"
        ) from error
    raise ImportError(
        "PyKelvin is not built. Build it with:\n"
        "  cmake -S . -B build -G Ninja && ninja -C build PyKelvin\n"
        f"(looked in {' and '.join(str(d) for d in _SEARCHED)})"
    ) from error

from mcp.server.fastmcp import FastMCP                 # noqa: E402
from mcp.server.transport_security import (            # noqa: E402
    TransportSecuritySettings,
)
from mcp.types import CallToolResult, TextContent      # noqa: E402

# --- what each family holds -------------------------------------------------
# Kelvin's own family list comes from the engine (kelvin.family_names()); this is
# only the one-line gloss and the selector's entry keys, which the engine has no
# way to describe. `requires` names the designRequirements keys Requirements.cpp
# reads for that family — the smoke test calls every one of them, so a key that
# stops being accepted fails there rather than misleading a caller here.
FAMILY_NOTES = {
    "mosfet": ("Si / SiC / GaN switches",
               ["ratedDrainSourceVoltage", "ratedContinuousDrainCurrent", "maximumOnResistance"]),
    "diode": ("schottky, fast-recovery, rectifier and zener diodes",
              ["ratedReverseVoltage", "ratedForwardCurrent"]),
    "capacitor": ("MLCC, electrolytic and film capacitors",
                  ["capacitance", "ratedVoltage", "minimumRippleCurrent"]),
    "resistor": ("chip, power and current-sense resistors",
                 ["resistance", "tolerance", "powerRating"]),
    "controller": ("PWM / PFC / resonant controller ICs",
                   ["topology (via options)", "inputVoltage (via options)",
                    "switchingFrequency (via options)"]),
    "igbt": ("IGBTs", ["ratedCollectorEmitterVoltage", "ratedCollectorCurrent"]),
    "bjt": ("bipolar transistors", ["ratedCollectorEmitterVoltage", "ratedCollectorCurrent"]),
    "varistor": ("MOV / TVS surge protectors", ["ratedVoltage", "maximumClampingVoltage"]),
    "magnetic": ("inductors, transformers, chokes and ferrite beads",
                 ["magnetizingInductance", "peakCurrent (via options)", "rmsCurrent (via options)"]),
    "analog": ("analog ICs — browse only, no selector yet", []),
    "timing": ("crystals, oscillators and resonators — browse only, no selector yet", []),
    "connector": ("connectors and terminal blocks",
                  ["positions", "family", "minimumCurrentPerContact", "minimumRatedVoltage"]),
}

# Families select() refuses: no requirements emitter, so no selection semantics to
# invent. They are fully browsable.
BROWSE_ONLY = ("analog", "timing")

UNITS_NOTE = (
    "All values are SI base units (F, H, V, A, Ω, Hz, s, C, m) except where the field name says "
    "otherwise: temp_min_c / temp_max_c are °C, tolerance is a fraction (0.01 = 1 %), positions "
    "is a count."
)

# --- MCP Apps wire constants (from @modelcontextprotocol/ext-apps 1.7.5) -----
# Every tool here that returns a ranked candidate list serves the SAME widget:
# search results, selector candidates and cross-reference substitutes are the
# same shape and the same human decision — pick one. (ABT #663 asks
# websharedcomponents for one such picker across Kirchhoff / OpenMagnetics /
# Hertz; this one is built to be replaced by it, not to compete with it.)
UI_RESOURCE_MIME = "text/html;profile=mcp-app"
UI_PICKER_URI = "ui://kelvin/picker.html"
UI_BUNDLES = {UI_PICKER_URI: Path(__file__).parent / "dist" / "picker.html"}


def _ui_meta(uri: str) -> dict:
    """registerAppTool() emits BOTH the flat key and the nested object, so hosts
    reading either form find it. Mirror that exactly."""
    return {"ui/resourceUri": uri, "ui": {"resourceUri": uri}}


UI_PICKER_META = _ui_meta(UI_PICKER_URI)


def assert_widgets_resolve() -> None:
    """Every ui:// this server advertises must have a bundle behind it.

    A tool that advertises a widget the host cannot fetch renders as a broken
    panel, and nothing server-side complains: OpenMagnetics ships a curves URI
    with no bundle and no build tooling, so eight tools advertise a UI that has
    never existed and nobody noticed (ABT #651). Refuse to start instead.
    """
    missing = [f"{uri} -> {path}" for uri, path in UI_BUNDLES.items() if not path.exists()]
    if missing:
        raise FileNotFoundError(
            "widget bundle(s) missing, so these tools would advertise a UI the host cannot "
            "fetch: " + "; ".join(missing) + " -- build them: cd mcp && npm install && npm run build")


# --- transport --------------------------------------------------------------
# The SDK's DNS-rebinding protection rejects an unrecognised Host with a bare
# "421 Invalid Host header", which remote hosts usually surface as a misleading
# sign-in failure. Behind a tunnel the Host is the PUBLIC name — name it here.
# Hertz uses 8400 and Kirchhoff 8401, so Kelvin takes 8402 and all three can run
# at once as separate connectors.
PORT = 8402
_public_host = os.environ.get("KELVIN_PUBLIC_HOST", "").strip()
if "://" in _public_host:                       # a pasted URL is fine; a Host has no scheme/path
    _public_host = _public_host.split("://", 1)[1]
_public_host = _public_host.split("/", 1)[0].strip()
if os.environ.get("KELVIN_ALLOW_ANY_HOST") == "1":
    _security = TransportSecuritySettings(enable_dns_rebinding_protection=False)
else:
    _allowed = [f"127.0.0.1:{PORT}", f"localhost:{PORT}", "127.0.0.1", "localhost"]
    if _public_host:
        _allowed += [_public_host, f"{_public_host}:443"]
    # allowed_origins is matched EXACTLY (or with a trailing ":*" port wildcard) — a bare "*" is
    # a literal that never matches, so it reads as "allow everything" while 403-ing every
    # browser-resident host. Name the origins that actually call.
    _origins = ["https://claude.ai", "https://www.claude.ai",
                "http://localhost:*", "http://127.0.0.1:*"]
    if _public_host:
        _origins.append(f"https://{_public_host}")
    _origins += [o.strip() for o in
                 os.environ.get("KELVIN_ALLOWED_ORIGINS", "").split(",") if o.strip()]
    _security = TransportSecuritySettings(allowed_hosts=_allowed, allowed_origins=_origins)

mcp = FastMCP("Kelvin", host="127.0.0.1", port=PORT, transport_security=_security)


# --- helpers ----------------------------------------------------------------

# --- the pipeline result contract (Moebius contracts/pipeline_result.json, ABT #685) --------
# Every tool result is a fixed DTO discriminated by `mode`, with one name per concept and no
# aliases. The rule that makes it worth the trouble: a consumer that sniffs for fields grows
# its own idea of what a payload means, and two pipelines then disagree. The schema is closed,
# so anything without a home in it must either earn a field or be dropped deliberately — never
# smuggled through under a second name.
#
# Only the STRUCTURED channel is constrained. The digest text stays free to say whatever the
# reader needs, which is where the nuance that has no DTO field (facet values, pool caps) goes.

# Candidate properties the contract names. Anything else on a candidate is pipeline-internal
# and travels with a leading underscore, which the contract reserves for exactly that.
_CANDIDATE_FIELDS = frozenset({
    "mpn", "manufacturer", "specs", "status", "grade", "penalty", "direction", "footprint",
    "params", "notes", "margins", "row", "sortKey", "evidence", "record",
})
# Row keys that identify or locate a part rather than describing it — never spec columns.
_ROW_INTERNALS = ("lineno", "line", "srcOffset", "srcLength")


def _no_nulls(obj: dict) -> dict:
    """Omit a field rather than sending null: absent means 'not applicable here', while null
    invites a consumer to render an empty value as if it were an answer."""
    return {k: v for k, v in obj.items() if v is not None}


def _candidate(row: dict, specs: dict | None = None) -> dict:
    """One catalogue row or ranked verdict as the contract's single candidate type.

    Parameters live under `specs` — never flat at candidate level, where they cannot be told
    apart from the engine's own judgement fields. Everything the contract does not name keeps
    its meaning under an underscore instead of being dropped.
    """
    out: dict = {"mpn": row.get("mpn")}
    if row.get("manufacturer") is not None:
        out["manufacturer"] = row["manufacturer"]
    if specs:
        out["specs"] = _no_nulls(specs)
    for key, value in row.items():
        if key in ("mpn", "manufacturer") or value is None:
            continue
        if key.startswith("_"):
            out[key] = value
        elif key in _CANDIDATE_FIELDS:
            out[key] = value
        else:
            out[f"_{key}"] = value
    return out


def _facets(page: dict) -> dict:
    """Facet counts in the contract's shape: value/count objects for categorical fields,
    min/max for numeric ones.

    The engine emits categorical facets as [value, count] pairs and numeric ones separately
    under `ranges`; both are one question to a caller ("what else is in this result set"), so
    they arrive as one map. An empty-string value is NOT padding — it is the engine saying the
    record does not state that field, and it is frequently the largest bucket, so it is passed
    through rather than filtered out.
    """
    out: dict = {}
    for name, facet in (page.get("facets") or {}).items():
        # An empty string is the engine saying the record does not state the field — often
        # the largest bucket in the result (187,217 of 217,864 capacitors state no dielectric
        # code). It travels as null so it cannot render as a blank chip a user might click.
        entry: dict = {"values": [{"value": v or None, "count": n}
                                  for v, n in facet.get("values") or []]}
        if facet.get("omitted"):
            entry["omitted"] = facet["omitted"]
        out[name] = entry
    for name, span in (page.get("ranges") or {}).items():
        # `present` is how many rows state the field at all: min=1e-13 max=3000 over 217,864
        # parts is a different statement from the same range over 12, and the reader cannot
        # tell which they have without it.
        if span.get("present"):
            out[name] = {"min": span.get("min"), "max": span.get("max"),
                         "present": span["present"]}
    return out


def _row_candidate(row: dict) -> dict:
    """A browse row: every field that is not identity or locator IS a spec."""
    specs = {k: v for k, v in row.items()
             if k not in ("mpn", "manufacturer") and k not in _ROW_INTERNALS}
    internals = {f"_{k}": row[k] for k in _ROW_INTERNALS if row.get(k) is not None}
    return _candidate({"mpn": row.get("mpn"), "manufacturer": row.get("manufacturer"),
                       **internals}, specs)


def _result(summary: str, payload: dict) -> CallToolResult:
    """Two channels: a compact digest for the model, the payload for a widget.

    Returning a plain dict from a FastMCP tool emits NO structuredContent and
    serialises the WHOLE payload into `content` — a 200-row catalogue page into
    the context window on one search. Every tool here builds its result
    explicitly, which FastMCP passes through verbatim.
    """
    return CallToolResult(content=[TextContent(type="text", text=summary)],
                          structuredContent=payload)


def _eng(value, unit: str = "") -> str:
    """Engineering notation, e.g. 4.7 nF / 100 k. Absent stays '-', never 0."""
    if value is None:
        return "-"
    value = float(value)
    if value == 0.0:
        return f"0 {unit}".strip()
    a = abs(value)
    for factor, prefix in ((1e-12, "p"), (1e-9, "n"), (1e-6, "µ"), (1e-3, "m"), (1.0, "")):
        if a < factor * 1000.0:
            return f"{value / factor:.4g} {prefix}{unit}".strip()
    if a < 1e6:
        return f"{value / 1e3:.4g} k{unit}".strip()
    if a < 1e9:
        return f"{value / 1e6:.4g} M{unit}".strip()
    return f"{value / 1e9:.4g} G{unit}".strip()


def _family(name: str) -> str:
    """Normalise + validate a family name against the engine's own list."""
    key = (name or "").strip().lower()
    known = kelvin.family_names()
    if key in known:
        return key
    if key.endswith("s") and key[:-1] in known:      # 'capacitors' -> 'capacitor'
        return key[:-1]
    raise ValueError(f"unknown family {name!r} -- one of: {', '.join(known)}")


def _data_dir() -> str:
    """The TAS NDJSON catalogue directory.

    Raises rather than returning an empty string: an engine with no catalogue
    reports "no candidates", which reads as "nothing fits" when the truth is
    "nobody looked".
    """
    resolved = os.environ.get("KELVIN_TAS_DATA_DIR", "").strip()
    if not resolved:
        raise ValueError(
            "no parts catalogue: set KELVIN_TAS_DATA_DIR to the directory holding the TAS NDJSON "
            "catalogues (capacitors.ndjson, mosfets.ndjson, ...)")
    if not Path(resolved).is_dir():
        raise ValueError(f"KELVIN_TAS_DATA_DIR {resolved!r} is not a directory")
    return resolved


def _index_dir() -> str:
    """Where the native engine keeps (and may rebuild) its .kidx shards.

    Deliberately NOT web/public/kelvin: those shards are the deployed site's and
    are shared with Kirchhoff, and a rebuild triggered by a data dir this server
    happens to point at would overwrite them.
    """
    resolved = os.environ.get("KELVIN_INDEX_DIR", "").strip() or str(Path.home() / ".kelvin/index")
    Path(resolved).mkdir(parents=True, exist_ok=True)
    return resolved


_engine = None
_engine_lock = threading.Lock()


def _eng_handle():
    """The process-wide engine. Shards load lazily, once, on first use of a family."""
    global _engine
    with _engine_lock:
        if _engine is None:
            _engine = kelvin.Engine(_data_dir(), _index_dir(), False)
        return _engine


def _browse(family: str, query: dict) -> dict:
    """browse(), with the family's own vocabulary attached to a bad-field error.

    An unknown filter name is the one error a caller can always fix itself, and
    the engine's message names the offending field but not the alternatives.
    """
    try:
        return _eng_handle().browse(family, query)
    except kelvin.InvalidOptions as error:
        if "unknown" in str(error) and "field" in str(error):
            raise ValueError(f"{error}. {family} accepts: "
                             f"{json.dumps(kelvin.family_fields(family))}, plus mpn (substring) "
                             f"and manufacturer (list of names).") from error
        raise


def _candidate_brief(row: dict) -> str:
    """One cross-reference verdict as an engineer would write it in a note.

    Names and scores alone are relayable but not reason-able: "penalty 0.14" does
    not tell a caller that the swap triples the gate charge, or that a parameter
    came back unverified because neither record states it. The concerns and the
    ranker's own prose are the part worth reading.
    """
    head = f"  {row.get('mpn')} ({row.get('manufacturer') or 'unknown maker'}): {row.get('status')}"
    if row.get("grade"):
        head += f" / {row['grade']}"
    if row.get("direction"):
        head += f" / {row['direction']}"
    if isinstance(row.get("penalty"), (int, float)):
        head += f", penalty {row['penalty']:.3f}"
    lines = [head]
    concerns = [f"{p.get('name')} {p.get('verdict')}" for p in row.get("params") or []
                if isinstance(p, dict) and p.get("verdict") in ("fail", "warn", "unverified")]
    if concerns:
        lines.append(f"      concerns: {', '.join(concerns)}")
    for note in (row.get("notes") or [])[:2]:
        lines.append(f"      note: {str(note)[:300]}")
    return "\n".join(lines)


def _row_digest(row: dict, numeric_fields: list[str], count: int = 4) -> str:
    """One catalogue row as a line: identity, then the family's leading specs."""
    specs = []
    for name in numeric_fields:
        if len(specs) >= count:
            break
        value = row.get(name)
        if value is not None:
            specs.append(f"{name} {_eng(value)}")
    return (f"  {row.get('mpn')} — {row.get('manufacturer')}"
            + (f" | {', '.join(specs)}" if specs else ""))


# --- the cross-reference worker ---------------------------------------------
# A long-lived Node process holding the WASM engine and the loaded shards. Started
# on first use (nothing pays for it if cross_reference is never called) and
# restarted if it dies, so one bad request cannot take the tool out for the
# session. See xref.mjs for why cross-reference goes through Node at all.

_xref_proc: subprocess.Popen | None = None
_xref_lock = threading.Lock()
_xref_id = 0
_xref_fingerprint: str | None = None
XREF_TIMEOUT_S = 300.0
# The worker's source: its own file plus the cross-reference pipeline it imports.
_XREF_SOURCES = (Path(__file__).parent / "xref.mjs",
                 _REPO / "web" / "src" / "crossref.js")


def _xref_source_fingerprint() -> str:
    """A hash of the code the worker SHOULD be running.

    A long-lived worker restarted only when it dies keeps serving the code it was started
    with, so an edit to the pipeline takes effect at some unrelated future restart and every
    answer until then is quietly stale — and the failure is invisible, because a stale worker
    answers perfectly well, just from the old rules. (Caught exactly this way: a worker 7.5
    hours older than the fix it was supposed to be running still returned the old payload,
    while a fresh one in a test returned the new one, and the two checks disagreed.)
    """
    digest = hashlib.sha256()
    for path in _XREF_SOURCES:
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def _xref_start() -> subprocess.Popen:
    shard_dir = (os.environ.get("KELVIN_SHARD_DIR", "").strip()
                 or str(_REPO / "web" / "public" / "kelvin"))
    if not list(Path(shard_dir).glob("*.kidx")):
        raise ValueError(
            f"no prebuilt shards in {shard_dir} -- cross_reference runs the web app's own "
            f"pipeline over them. Build them (web/scripts/build-kelvin-shards.sh) or point "
            f"KELVIN_SHARD_DIR at a directory holding <family>.kidx + <family>.ndjson.")
    proc = subprocess.Popen(
        ["node", str(Path(__file__).parent / "xref.mjs"), "--shards", shard_dir],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None,
        text=True, bufsize=1)
    hello = proc.stdout.readline()
    handshake = json.loads(hello) if hello else {}
    if not handshake.get("ready"):
        proc.kill()
        raise RuntimeError("the cross-reference worker did not start (see its stderr above)")
    # The worker hashes its own source; we hash what is on disk. A disagreement means the
    # files changed while it was starting, and the worker we just got is not the one we
    # think we started.
    expected = _xref_source_fingerprint()
    if handshake.get("fingerprint") != expected:
        proc.kill()
        raise RuntimeError(
            f"the cross-reference worker reports source {handshake.get('fingerprint')!r} but "
            f"the files on disk hash to {expected!r} -- they changed mid-start; try again")
    return proc


def _xref(request: dict) -> dict:
    """One request/response round-trip with the worker, restarting it if it died.

    The worker is also restarted when its SOURCE changes. Without that, a fix to xref.mjs or
    to the crossref pipeline it imports only lands whenever the worker next happens to die,
    and until then the tool keeps answering from the old code — correctly-shaped answers from
    superseded rules, which is the hardest kind of wrong to notice.
    """
    global _xref_proc, _xref_id, _xref_fingerprint
    with _xref_lock:
        current = _xref_source_fingerprint()
        if _xref_proc is not None and _xref_proc.poll() is None and _xref_fingerprint != current:
            _xref_proc.terminate()
            try:
                _xref_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:                   # pragma: no cover
                _xref_proc.kill()
            _xref_proc = None
        if _xref_proc is None or _xref_proc.poll() is not None:
            _xref_proc = _xref_start()
            _xref_fingerprint = current
        _xref_id += 1
        request = {**request, "id": _xref_id}
        try:
            _xref_proc.stdin.write(json.dumps(request) + "\n")
            _xref_proc.stdin.flush()
            line = _xref_proc.stdout.readline()
        except (BrokenPipeError, ValueError) as error:
            _xref_proc = None
            raise RuntimeError(f"the cross-reference worker died mid-request: {error}") from error
        if not line:
            _xref_proc = None
            raise RuntimeError("the cross-reference worker closed its output (it died)")
        reply = json.loads(line)
    if not reply.get("ok"):
        raise ValueError(reply.get("error") or "cross-reference failed")
    return reply["result"]


# --- tools ------------------------------------------------------------------

@mcp.tool(
    title="List catalogue families",
    description=(
        "Every component family in the TAS catalogue, with how many parts it holds and whether "
        "it has a deterministic selector. Start here."
    ),
    structured_output=False,
)
def list_families() -> CallToolResult:
    """The catalogue's table of contents.

    Deliberately does NOT report part counts: the count lives in a family's index
    shard, and reading all twelve to answer "what is there?" would load ~800 MB
    for a question that never needed it. describe_family loads the one asked for.
    """
    families = []
    lines = []
    for name in kelvin.family_names():
        note, requires = FAMILY_NOTES.get(name, ("", []))
        families.append({"family": name, "holds": note,
                         "selector": name not in BROWSE_ONLY,
                         "requirementKeys": requires})
        lines.append(f"  {name}: {note}"
                     + ("" if name not in BROWSE_ONLY else "  [browse only]"))
    return _result(
        f"{len(families)} component families:\n" + "\n".join(lines)
        + "\n\ndescribe_family gives one family's part count and the exact fields it can be "
          "searched on.\n" + UNITS_NOTE,
        {"mode": "catalogue", "families": families, "units": UNITS_NOTE})


@mcp.tool(
    title="Describe a family's query vocabulary",
    description=(
        "The exact field names one family can be filtered, sorted and bucketed on, and the "
        "designRequirements keys its selector reads. Call this before search_parts on an "
        "unfamiliar family rather than guessing field names."
    ),
    structured_output=False,
)
def describe_family(family: str) -> CallToolResult:
    """The browse vocabulary of one family.

    Args:
        family: 'capacitor', 'mosfet', 'magnetic', ... (see list_families).
    """
    key = _family(family)
    fields = kelvin.family_fields(key)
    note, requires = FAMILY_NOTES.get(key, ("", []))
    page = _browse(key, {"limit": 0})
    return _result(
        f"{key} — {note}, {page.get('rowCount'):,} parts.\n"
        f"  numeric (filter {{min,max}}, sort, distribution): {', '.join(fields['numeric'])}\n"
        f"  categorical (filter as a list of values, facetable): "
        f"{', '.join(fields['string'] + fields['list']) or '(none)'}\n"
        f"  boolean: {', '.join(fields['boolean']) or '(none)'}\n"
        f"  always available: mpn (substring match), manufacturer (list of names)\n"
        + (f"  recommend_parts reads: {', '.join(requires)}\n" if requires
           else "  no selector — this family is browse-only.\n")
        + UNITS_NOTE,
        # `string` and `list` fields are one thing to a caller — both are filtered with an
        # array of values and both are facetable — so the contract's `categorical` covers
        # them. The scalar/list distinction is an implementation detail of the shard.
        {"mode": "fields", "family": key,
         "fields": {"numeric": fields["numeric"],
                    "categorical": fields["string"] + fields["list"],
                    "boolean": fields["boolean"]},
         "requires": requires, "selector": key not in BROWSE_ONLY,
         "catalogueTotal": page.get("rowCount"), "units": UNITS_NOTE})


@mcp.tool(
    title="Search the parts catalogue",
    description=(
        "Parametric search over one family: numeric ranges, categorical values, MPN substring "
        "and manufacturer, with optional facet counts. This is the catalogue question — 'what "
        "parts do we have that ...' — and needs no converter design."
    ),
    meta=UI_PICKER_META,
    structured_output=False,
)
def search_parts(family: str, filters: dict | None = None, sort: dict | None = None,
                 limit: int = 25, offset: int = 0, with_facets: bool = False) -> CallToolResult:
    """Parametric catalogue search.

    Args:
        family: 'capacitor', 'mosfet', 'magnetic', ... (see list_families).
        filters: {"<numeric field>": {"min": x, "max": y}, "<categorical field>": ["v1","v2"],
            "<boolean field>": true, "mpn": "substring", "manufacturer": ["Vishay"]}.
            Field names come from describe_family; an unknown one is an error, never ignored.
        sort: {"field": "<numeric field>|mpn|manufacturer|lineno", "dir": "asc"|"desc"}.
        with_facets: also return the value counts of every categorical field and the min/max of
            every numeric one, each counted over all the OTHER filters (so narrowing one facet
            never empties its own option list).
    """
    key = _family(family)
    query: dict = {"limit": max(0, min(int(limit), 200)), "offset": max(0, int(offset))}
    if filters:
        query["filters"] = filters
    if sort:
        query["sort"] = sort
    if with_facets:
        query["withFacets"] = True
        query["facetTop"] = 25
    page = _browse(key, query)
    numeric = kelvin.family_fields(key)["numeric"]
    rows = page.get("rows") or []
    shown = f"{len(rows)} shown" if len(rows) < page["total"] else "all shown"
    summary = (f"{page['total']:,} {key}(s) match ({shown}, from {page['rowCount']:,} in the "
               f"catalogue):\n" + "\n".join(_row_digest(r, numeric) for r in rows[:40]))
    if not rows:
        summary = (f"No {key} matches those filters (the family holds {page['rowCount']:,} "
                   f"parts). A part whose record does not state a field you filtered on is "
                   f"excluded by that filter — widen it, or drop it and read the facet counts.")
    if with_facets:
        # The result contract has no field for facet counts, so they are reported in full
        # HERE rather than quietly dropped: the values and their counts are the whole point
        # of asking for them, and a reader who cannot see them cannot narrow the search.
        for name, facet in sorted((page.get("facets") or {}).items()):
            values = facet.get("values") or []
            if not values:
                continue
            summary += (f"\n{name}: "
                        + ", ".join(f"{v} ({n:,})" for v, n in values[:8])
                        + (f", +{len(values) - 8} more" if len(values) > 8 else "")
                        + (f" [{facet['omitted']} rarer values omitted]"
                           if facet.get("omitted") else ""))
    return _result(summary, _no_nulls({
        "mode": "search",
        "family": key,
        "candidates": [_row_candidate(r) for r in rows],
        "total": page.get("total"),                 # matched this query
        "catalogueTotal": page.get("rowCount"),     # the family's size — a different fact
        "shown": len(rows),
        "filters": filters or None,
        "facets": _facets(page) or None,
    }))


@mcp.tool(
    title="Look up one part",
    description=(
        "One part by MPN: its extracted specs and, optionally, the full TAS catalogue record "
        "behind them (datasheet URL, mechanical drawing, provenance)."
    ),
    structured_output=False,
)
def part_details(family: str, mpn: str, manufacturer: str | None = None,
                 include_record: bool = True) -> CallToolResult:
    """One part, by MPN.

    Args:
        mpn: the manufacturer part number (substring match; an exact hit wins).
        manufacturer: needed only when two vendors ship the same MPN string.
        include_record: include the raw TAS record, not just the extracted specs.
    """
    key = _family(family)
    page = _browse(key, {"filters": {"mpn": mpn}, "limit": 50})
    rows = page.get("rows") or []
    if manufacturer:
        want = manufacturer.strip().lower()
        rows = [r for r in rows if want in (r.get("manufacturer") or "").lower()]
    exact = [r for r in rows if (r.get("mpn") or "").lower() == mpn.strip().lower()]
    rows = exact or rows
    if not rows:
        raise ValueError(
            f"no {key} whose MPN contains {mpn!r}"
            + (f" from a manufacturer matching {manufacturer!r}" if manufacturer else "")
            + f" ({page['total']} row(s) matched the MPN before any manufacturer filter)")
    if len(rows) > 1:
        # An ambiguous MPN is a short search, not a broken lookup — so it comes back as one,
        # with the same candidate type, instead of a bespoke {ambiguous: true} shape the
        # caller would have to learn.
        listing = "\n".join(f"  {r['mpn']} — {r['manufacturer']}" for r in rows[:15])
        return _result(
            f"{len(rows)} {key}s match {mpn!r} — name the exact MPN (or the manufacturer):\n"
            + listing,
            {"mode": "search", "family": key,
             "candidates": [_row_candidate(r) for r in rows[:15]],
             "total": page["total"], "shown": min(len(rows), 15)})

    row = rows[0]
    numeric = kelvin.family_fields(key)["numeric"]
    record = (_eng_handle().fetch_record(key, row["srcOffset"], row["srcLength"])
              if include_record else None)
    part = _row_candidate(row)
    if record is not None:
        part["record"] = record
    payload = {"mode": "details", "family": key, "part": part}
    specs = "\n".join(f"  {n}: {_eng(row.get(n))}" for n in numeric if row.get(n) is not None)
    cats = ", ".join(f"{n}={row[n]}" for n in kelvin.family_fields(key)["string"] if row.get(n))
    return _result(
        f"{row['mpn']} ({row['manufacturer']}) — {key}\n{specs}"
        + (f"\n  {cats}" if cats else "")
        + (f"\n  case {row.get('caseCode')}" if row.get("caseCode") else "")
        + ("\n(the full TAS record is in the structured output)" if include_record else ""),
        payload)


@mcp.tool(
    title="Recommend parts for a requirement",
    description=(
        "Kelvin's deterministic selector: rank real parts against a designRequirements block "
        "and return the candidates with the margins that ranked them. Same authority Kirchhoff "
        "and Heaviside select through — no LLM judgement, reproducible."
    ),
    meta=UI_PICKER_META,
    structured_output=False,
)
def recommend_parts(family: str, requirements: dict, options: dict | None = None,
                    max_results: int = 12, include_envelope: bool = False) -> CallToolResult:
    """Ranked candidates for one component requirement.

    Args:
        requirements: that family's designRequirements block, SI units. The keys each family
            reads are listed by describe_family (e.g. mosfet: ratedDrainSourceVoltage,
            ratedContinuousDrainCurrent, maximumOnResistance).
        options: selector options — tiebreaker, excludeDiscontinued, manufacturerAllowlist,
            maxManufacturerFraction, switchingFrequency, and per-family extras (mosfet qgMax /
            technologyAllowed, diode qrrMax, capacitor capacitanceMin|Max, resistor
            maxValueDeviation|maxTolerance, controller topology + inputVoltage +
            switchingFrequency, magnetic peakCurrent|rmsCurrent).
        include_envelope: attach each candidate's full datasheet record (large).
    """
    key = _family(family)
    if key in BROWSE_ONLY:
        raise ValueError(
            f"'{key}' has no selector — it is a browse-only family (no requirements emitter "
            f"exists for it yet). Use search_parts.")
    opts = dict(options or {})
    opts["maxCandidates"] = max(1, min(int(max_results), 50))
    opts["includeEnvelope"] = bool(include_envelope)
    try:
        result = _eng_handle().select(key, requirements, opts)
    except kelvin.NoCandidates as error:
        # The rejection histogram is the answer to "why nothing?" — it says which gate did the
        # rejecting, so the caller can relax THAT one instead of guessing.
        detail = json.loads(str(error).split("\n")[0]) if str(error).startswith("{") else {}
        buckets = detail.get("rejections") or {}
        raise ValueError(
            f"no {key} satisfies those requirements "
            f"({detail.get('totalRowsConsidered', '?')} parts considered). Rejections by gate: "
            + (", ".join(f"{k}={v}" for k, v in sorted(buckets.items(), key=lambda kv: -kv[1]))
               or "(none recorded)")) from error
    cands = result.get("candidates") or []
    lines = []
    for c in cands[:15]:
        # A margin the record could not state is null, not 0 — it has no place in a
        # "tightest margin" comparison, and ranking it as the smallest would report an
        # absent datum as the binding constraint.
        margins = [(n, v) for n, v in (c.get("margins") or {}).items()
                   if isinstance(v, (int, float))]
        worst = min(margins, key=lambda kv: kv[1]) if margins else None
        lines.append(f"  {c.get('mpn')} — {c.get('manufacturer')}"
                     + (f" | tightest margin {worst[0]} {worst[1]:.2f}x" if worst else ""))
    rejected = result.get("rejections") or {}
    return _result(
        f"{len(cands)} {key} candidate(s), best first "
        f"(tiebreaker {result.get('tiebreaker', 'n/a')}). "
        f"{result.get('alternativesConsidered', 0):,} of "
        f"{result.get('totalRowsConsidered', 0):,} parts satisfied the requirements"
        + (", rejections by gate: "
           + ", ".join(f"{k}={v}" for k, v in sorted(rejected.items(), key=lambda kv: -kv[1])[:6])
           if rejected else "")
        + ":\n" + "\n".join(lines),
        _no_nulls({
            "mode": "recommend",
            "family": key,
            "candidates": [_candidate(c) for c in cands],
            # `total` is what matched the requirements; the whole family is a separate fact
            # under a name that says which is which.
            "total": result.get("alternativesConsidered"),
            "catalogueTotal": result.get("totalRowsConsidered"),
            "shown": len(cands),
            "tiebreaker": result.get("tiebreaker"),
            # Why a part is NOT here, per gate. `total` already says how many survived, so
            # the two are complementary rather than two names for one count.
            "rejections": rejected or None,
        }))


@mcp.tool(
    title="Distribution of a spec",
    description=(
        "How one numeric spec is distributed across a filtered slice of a family — bucket counts "
        "(log or linear) plus how many parts state the value at all. Answers 'what is available' "
        "questions that a top-N list cannot."
    ),
    structured_output=False,
)
def spec_distribution(family: str, field: str, filters: dict | None = None,
                      buckets: int = 20, log: bool = True) -> CallToolResult:
    """Histogram of one numeric field over a filtered set.

    Args:
        field: a numeric field of that family (see describe_family).
        log: log10 buckets (right for capacitance, resistance, inductance); False for linear
            (right for voltage, temperature, position count).
    """
    key = _family(family)
    query: dict = {"limit": 0, "histogram": {"field": field, "buckets": max(1, min(buckets, 60)),
                                             "log": bool(log)}}
    if filters:
        query["filters"] = filters
    page = _browse(key, query)
    hist = page.get("histogram") or {}
    edges, counts = hist.get("edges") or [], hist.get("counts") or []
    lines = []
    peak = max(counts) if counts else 0
    for i, n in enumerate(counts):
        if not n:
            continue
        bar = "#" * max(1, round(20 * n / peak)) if peak else ""
        lines.append(f"  {_eng(edges[i])} .. {_eng(edges[i + 1])}: {n:>7,} {bar}")
    return _result(
        f"{field} across {page['total']:,} matching {key}(s): {hist.get('present', 0):,} state it, "
        f"{hist.get('absent', 0):,} do not "
        f"({'log' if hist.get('log') else 'linear'} buckets)\n" + "\n".join(lines),
        _no_nulls({
            "mode": "distribution",
            "family": key,
            "field": field,
            "histogram": {k: v for k, v in hist.items() if k not in ("present", "absent")},
            "present": hist.get("present"),
            "absent": hist.get("absent"),
            "filters": filters or None,
        }))


@mcp.tool(
    title="Cross-reference a part",
    description=(
        "Find substitutes for a part by MPN — the deterministic, program-only cross-reference "
        "the Kelvin web app runs: pre-gate the catalogue at the ranker's own reject edges, then "
        "score every candidate on value, ratings, footprint and mount type. No LLM judgement. "
        "An original whose own specs are incomplete can never yield a clean 'recommended'."
    ),
    meta=UI_PICKER_META,
    structured_output=False,
)
def cross_reference(family: str, mpn: str, manufacturer: str | None = None,
                    target_manufacturers: list[str] | None = None,
                    same_type: bool = True, max_results: int = 12) -> CallToolResult:
    """Scored substitutes for one catalogue part.

    Args:
        mpn: the original part number (it must be in the catalogue — look it up with
            part_details first if unsure).
        target_manufacturers: restrict substitutes to these vendors; omitted, every OTHER
            vendor in the family is a target.
        same_type: keep candidates of the original's own type (technology / device type /
            connector family). False widens the search across types.
    """
    key = _family(family)
    result = _xref({"op": "crossref", "family": key, "mpn": mpn, "manufacturer": manufacturer,
                    "manufacturers": target_manufacturers or [],
                    "sameType": bool(same_type), "maxResults": max(1, min(max_results, 40))})
    original = result["original"]
    ranked = result.pop("ranked")
    lines = []
    for c in ranked:
        # The worker carries each candidate's shard row alongside the verdict; the maker
        # lives there, not in the ranker's output.
        c.setdefault("manufacturer", (c.get("row") or {}).get("manufacturer"))
        lines.append(_candidate_brief(c))
    header = (f"{len(ranked)} substitute(s) for {original['mpn']} ({original['manufacturer']}), "
              f"best first — {result['poolScored']:,} of {result['poolTotal']:,} pre-gated "
              f"candidates scored")
    if result["poolScored"] < result["poolTotal"]:
        header += f" (pool capped at {result['poolLimit']:,})"

    # The honesty cap is the single most important thing this tool says, so it must survive
    # into the DTO and not only into the prose. The contract has one field for a standing
    # warning — `caveat` — so the cap goes there, ahead of the family's own caveat, rather
    # than being dropped for want of a dedicated flag.
    caveats = []
    if not result["origVerified"]:
        caveats.append(
            f"The original's own record does not state {', '.join(result['missing'])}, so no "
            f"candidate can be graded 'recommended' (the honesty cap).")
    if result.get("caveat"):
        caveats.append(result["caveat"])
    if result["poolScored"] < result["poolTotal"]:
        caveats.append(f"{result['poolScored']:,} of {result['poolTotal']:,} pre-gated "
                       f"candidates were scored (pool capped at {result['poolLimit']:,}).")
    if caveats:
        header += "\n" + "\n".join(caveats)
    if result.get("targetsFromFacet"):
        header += f"\nTargets: every other vendor in the family ({len(result['targets'])})."

    return _result(header + ":\n" + "\n".join(lines), _no_nulls({
        "mode": "crossref",
        "family": key,
        "candidates": [_candidate(c) for c in ranked],
        "original": _row_candidate(original),
        "originalSpecs": _no_nulls(result.get("origSpec") or {}) or None,
        "total": result.get("poolTotal"),
        "shown": len(ranked),
        # `preGated` is deliberately NOT sent: it is the same number as `total`, and one fact
        # under two names is what this contract exists to prevent.
        "pool": {"scored": result["poolScored"],
                 **({"cap": result["poolLimit"]}
                    if result["poolScored"] < result["poolTotal"] else {})},
        # The honesty cap, machine-readable: a consumer that refuses to present an unverified
        # cross-reference can now tell by inspection instead of parsing the caveat.
        "originalVerified": result.get("origVerified"),
        "missingSpecs": result.get("missingKeys") or None,
        "caveat": " ".join(caveats) or None,
    }))


# --- the MCP Apps UI resource -----------------------------------------------

@mcp.resource(
    UI_PICKER_URI,
    name="kelvin-picker",
    title="Kelvin candidate picker",
    mime_type=UI_RESOURCE_MIME,
)
def picker_widget() -> str:
    """Ranked-candidate picker: sortable spec table, per-parameter verdict pills,
    the ranker's notes, and a 'use this' action that reports the choice back to
    the model. Shared by search_parts, recommend_parts and cross_reference —
    they all return the same shape and pose the same question.

    MCP App resources render in a deny-by-default CSP iframe, so the widget is
    built as ONE self-contained file (vite-plugin-singlefile).
    """
    bundle = UI_BUNDLES[UI_PICKER_URI]
    if not bundle.exists():                                     # pragma: no cover
        raise FileNotFoundError(
            f"{bundle} missing -- build the widget first: cd mcp && npm install && npm run build")
    return bundle.read_text(encoding="utf-8")


def build_app():
    """Starlette app with CORS.

    Browser-resident MCP hosts fetch /mcp from page JavaScript, so without these headers the
    connection dies at the preflight — and the streamable transport additionally needs to READ
    `Mcp-Session-Id` off the response, which cross-origin JS cannot do unless it is exposed.
    """
    from starlette.middleware.cors import CORSMiddleware

    assert_widgets_resolve()
    app = mcp.streamable_http_app()
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],          # tighten to your host origins in production
        allow_methods=["GET", "POST", "DELETE", "OPTIONS"],
        allow_headers=["*"],
        expose_headers=["Mcp-Session-Id"],
    )
    return app


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(build_app(), host=mcp.settings.host, port=mcp.settings.port)
