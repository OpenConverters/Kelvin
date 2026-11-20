"""/health answers while a slow tool call is in flight — over HTTP, against the real ASGI app.

What went wrong on prod (2026-11-15 20:34 and 20:36): FastMCP (mcp 1.27) runs a plain sync tool
function ON the event loop, so a cold shard load (~20 s) froze the whole server. /health stopped
answering, and the health timer (two failures 20 s apart) restarted the service mid-call. The
fix registers each sync tool as an async twin that runs the body in a worker thread
(server._sync_tool).

This starts the real app (build_app under uvicorn) on a spare port with the engine call behind
describe_family (server._browse) replaced by one that sleeps SLOW seconds, calls describe_family
over MCP, and hits /health while that call is still running. With the old dispatch, /health
waits for the whole sleep and the test fails.

    python3 -m pytest mcp/test_health_while_busy.py     (needs the built PyKelvin and mcp/dist)
"""
from __future__ import annotations

import asyncio
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import httpx
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

HERE = Path(__file__).resolve().parent
SLOW = 4.0          # seconds the patched engine call takes
HEALTH_MAX = 1.0    # /health must answer within this while the call runs

# Runs in the server process: the engine call becomes a sleep, so the test needs no catalogue
# and the slow call is deterministic. Everything else — registration, dispatch, routes — is the
# server's own.
_SERVER = """
import sys, time
import server, uvicorn

def slow_browse(family, query):
    time.sleep({slow})
    return {{"rowCount": 1}}

server._browse = slow_browse
server._catalogue_drift = lambda family, rows: None
uvicorn.run(server.build_app(), host="127.0.0.1", port={port}, log_level="warning")
"""


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def _probe(base: str) -> tuple[float, int, bool, object, float]:
    async with streamablehttp_client(base + "/mcp") as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()
            started = time.monotonic()
            call = asyncio.create_task(
                session.call_tool("describe_family", {"family": "capacitor"}))
            await asyncio.sleep(0.5)                       # the call is now inside the sleep
            async with httpx.AsyncClient() as http:
                t0 = time.monotonic()
                try:
                    response = await http.get(base + "/health", timeout=SLOW + 5)
                    status = response.status_code
                except httpx.HTTPError:
                    status = -1
                health_s = time.monotonic() - t0
            pending = not call.done()
            result = await call
            return health_s, status, pending, result, time.monotonic() - started


def test_health_answers_while_a_tool_call_runs():
    port = _free_port()
    with tempfile.TemporaryDirectory() as tmp:
        env = {**os.environ, "KELVIN_ALLOW_ANY_HOST": "1", "KELVIN_WORK_DIR": tmp,
               # /health only asks that the catalogue directory exists; the engine is patched.
               "KELVIN_TAS_DATA_DIR": tmp, "KELVIN_SHARD_DIR": tmp}
        for key in ("KELVIN_USAGE_LOG", "KELVIN_TELEMETRY_URL", "KELVIN_UMAMI_URL",
                    "KELVIN_PUBLIC_MODE"):
            env.pop(key, None)
        proc = subprocess.Popen([sys.executable, "-c", _SERVER.format(slow=SLOW, port=port)],
                                cwd=HERE, env=env)
        try:
            for _ in range(150):
                assert proc.poll() is None, f"the server exited with {proc.returncode}"
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.2)
            else:
                raise AssertionError("the server did not start listening")
            health_s, status, pending, result, call_s = asyncio.run(
                _probe(f"http://127.0.0.1:{port}"))
        finally:
            proc.terminate()
            proc.wait(timeout=20)

    assert status == 200, f"/health answered {status}"
    assert health_s < HEALTH_MAX, (
        f"/health took {health_s:.2f} s while a {SLOW:.0f} s tool call ran: the call is "
        f"blocking the event loop")
    assert pending, "the tool call had already finished when /health answered — nothing was probed"
    assert not result.isError, f"describe_family failed: {result.content}"
    assert call_s >= SLOW, f"the call took {call_s:.2f} s, less than the patched {SLOW} s sleep"
