"""A large BOM never makes the rest of Kelvin wait — checked over MCP, against a real server.

What went wrong on the demo host: a 1,908-line quote BOM ran inside crossref_bom for 6+
minutes. FastMCP runs a plain function ON the event loop, so the whole server — tools/list
included — froze for the run, every host's health check of Kelvin hung, and every single-part
cross_reference queued behind the BOM on the one worker.

This starts the real server (real worker, real shards) on a spare port and checks:
  1. a BOM over BOM_SYNC_MAX_LINES returns a job envelope at once (mode "job");
  2. while that job runs, tools/list and a single-part cross_reference each answer in < 1 s;
  3. the job finishes; job_status says done; job_result carries a `bom` result with every
     line and names the table widget;
  4. a small BOM is still answered directly (mode "bom"), and while one at the limit
     computes, tools/list still answers in < 1 s.

    python3 mcp/test_bomjob.py            (needs Node, built PyKelvin and prebuilt shards)

Not pytest: it is minutes of real work, run on purpose, like smoke.py.
"""
from __future__ import annotations

import asyncio
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

HERE = Path(__file__).resolve().parent
LINES = 600              # distinct lines; ~40 s of batch work here, so there is time to probe
FAILS: list[str] = []


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if ok else 'FAIL'}  {label}" + (f" — {detail}" if detail else ""))
    if not ok:
        FAILS.append(label)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def write_bom(path: Path, n: int) -> None:
    # Part numbers nobody makes: the costly path (every family scanned for each), and
    # deterministic. Designator + part-number headers are in Kelvin's own vocabulary, so the
    # file is read without asking Jev.
    rows = ["Designator,Part Number,Quantity"]
    rows += [f"U{i},QZX{i:05d}NOPART,1" for i in range(1, n + 1)]
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")


# Timed from BEFORE the connection: a server whose event loop is blocked accepts nothing, so
# timing only the request after the handshake would measure the wrong thing — the handshake
# is where a frozen server makes a host wait.
async def call(url: str, tool: str, args: dict):
    t0 = time.monotonic()
    async with streamablehttp_client(url) as (r, w, _):
        async with ClientSession(r, w) as s:
            await s.initialize()
            res = await s.call_tool(tool, args)
            return res, time.monotonic() - t0


async def list_tools(url: str):
    t0 = time.monotonic()
    async with streamablehttp_client(url) as (r, w, _):
        async with ClientSession(r, w) as s:
            await s.initialize()
            tools = (await s.list_tools()).tools
            return tools, time.monotonic() - t0


def text(res) -> str:
    return "\n".join(c.text for c in res.content if getattr(c, "type", "") == "text")


async def main(url: str, work: Path) -> None:
    big = work / "big_bom.csv"
    write_bom(big, LINES)

    print("warm the interactive worker (its first start loads shards)")
    probe_part = {"family": "capacitor", "mpn": "885012106006", "max_results": 3}
    res, dt = await call(url, "cross_reference", probe_part)
    check("single-part cross_reference answers", not res.isError, text(res)[:120])

    print(f"crossref_bom with {LINES} distinct lines")
    res, dt = await call(url, "crossref_bom", {"bom": str(big),
                                               "target_manufacturers": ["Würth"]})
    sc = res.structuredContent or {}
    check("is submitted as a job, not computed in the call", sc.get("mode") == "job",
          f"mode {sc.get('mode')!r}: {text(res)[:200]}")
    check("the submission returns at once (< 5 s)", dt < 5, f"{dt:.2f} s")
    job = sc.get("job")
    if not job:
        return

    print("while the job runs")
    await asyncio.sleep(3)        # past the batch worker's start, into the scanning
    _, dt_list = await list_tools(url)
    check("tools/list answers in < 1 s", dt_list < 1, f"{dt_list:.2f} s")
    res, dt_x = await call(url, "cross_reference", probe_part)
    check("a single-part cross_reference answers in < 1 s", not res.isError and dt_x < 1,
          f"{dt_x:.2f} s")
    st, _ = await call(url, "job_status", {"job": job})
    check("the job is still running while those answered",
          st.structuredContent.get("state") in ("queued", "running"),
          json.dumps(st.structuredContent)[:200])

    t0 = time.monotonic()
    while True:
        st, _ = await call(url, "job_status", {"job": job})
        state = st.structuredContent.get("state")
        if state not in ("queued", "running"):
            break
        if time.monotonic() - t0 > 900:
            check("the job finishes within 15 min", False)
            return
        await asyncio.sleep(2)
    print(f"job {job} ended {state} after {time.monotonic() - t0:.0f} s more")
    check("the job is done", state == "done", st.structuredContent.get("error") or "")
    if state != "done":
        return
    res, _ = await call(url, "job_result", {"job": job})
    sc = res.structuredContent or {}
    result = sc.get("result") or {}
    check("job_result is the job envelope carrying a `bom` result",
          sc.get("mode") == "job" and result.get("mode") == "bom",
          f"{sc.get('mode')} / {result.get('mode')}")
    check(f"the result accounts for every one of the {LINES} lines",
          result.get("total") == LINES, f"total {result.get('total')}")
    tools, _ = await list_tools(url)
    meta = {t.name: (t.meta or {}).get("ui/resourceUri") for t in tools}
    check("job_result draws the cross-reference table",
          meta.get("job_result") == "ui://kelvin/crossref-table.html", str(meta.get("job_result")))

    print("a small BOM")
    res, dt = await call(url, "crossref_bom", {"bom": str(HERE / "fixtures/bom/kicad_bom.csv")})
    check("is answered directly", (res.structuredContent or {}).get("mode") == "bom",
          text(res)[:160])

    print("a BOM at the synchronous limit, answered in the call")
    edge = work / "edge_bom.csv"
    write_bom(edge, 150)
    answer = asyncio.create_task(call(url, "crossref_bom", {"bom": str(edge)}))
    await asyncio.sleep(2)
    _, dt_list = await list_tools(url)
    res, dt = await answer
    check("is answered directly", (res.structuredContent or {}).get("mode") == "bom",
          text(res)[:120])
    check("tools/list answers in < 1 s while it computes (off the event loop)",
          dt_list < 1 and dt > 2, f"tools/list {dt_list:.2f} s during a {dt:.1f} s call")


if __name__ == "__main__":
    port = free_port()
    with tempfile.TemporaryDirectory() as tmp:
        env = {**os.environ, "KELVIN_ALLOW_ANY_HOST": "1", "KELVIN_WORK_DIR": tmp}
        server = subprocess.Popen(
            [sys.executable, "-c",
             f"import server, uvicorn; uvicorn.run(server.build_app(), host='127.0.0.1', "
             f"port={port}, log_level='warning')"], cwd=HERE, env=env)
        try:
            for _ in range(100):
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.2)
            asyncio.run(main(f"http://127.0.0.1:{port}/mcp", Path(tmp)))
        finally:
            server.terminate()
            server.wait(timeout=20)
    print()
    print(f"{len(FAILS)} FAILED: " + "; ".join(FAILS) if FAILS else "all BOM-job checks passed")
    sys.exit(1 if FAILS else 0)
