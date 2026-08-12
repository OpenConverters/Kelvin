# Kelvin as an MCP App

Exposes the Kelvin parts catalogue as [MCP](https://modelcontextprotocol.io) tools, plus a
ranked-candidate **picker** as an [MCP Apps](https://modelcontextprotocol.io/extensions/apps/build)
(SEP-1865) UI resource.

The point: catalogue questions do not need a converter design to exist first. *"What film caps do
we have between 100 nF and 470 nF rated over 300 V?"*, *"what does this competitor part
cross-reference to?"*, *"how is Rds(on) distributed across 100 V silicon?"* are the daily
ADM/FAE questions, and until now the only way to reach Kelvin from an assistant was through
Kirchhoff's `select_parts` — a design-oriented slice of it. (ABT #666.)

No LLM enters this server. Every verdict is Kelvin's deterministic C++; the server is a
transport, not a second opinion.

Hosts that understand MCP Apps (Claude web/Desktop, ChatGPT, VS Code Copilot) render the picker.
Hosts that speak only plain MCP still get the tools and the text answer — the `_meta` they do not
understand is ignored.

## Build and run

```bash
cmake -S . -B build -G Ninja && ninja -C build PyKelvin     # the engine
cd mcp && npm install && npm run build && cd ..             # the widget

KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 mcp/server.py # 127.0.0.1:8402/mcp
```

Python needs `mcp`, `uvicorn`, `starlette` and a built `PyKelvin`. `cross_reference` also needs
`node` and prebuilt shards. The widget bundle must exist before the server starts — it raises
rather than advertising a UI the host cannot fetch (see *Widgets*).

| Variable | Meaning | Default |
|---|---|---|
| `KELVIN_TAS_DATA_DIR` | the TAS NDJSON catalogue (`capacitors.ndjson`, `mosfets.ndjson`, …) | **required** |
| `KELVIN_INDEX_DIR` | where the native engine keeps (and may rebuild) its `.kidx` shards | `~/.kelvin/index` |
| `KELVIN_SHARD_DIR` | prebuilt `<family>.kidx` + `<family>.ndjson` the cross-reference worker reads | `web/public/kelvin` |
| `KELVIN_PUBLIC_HOST` / `KELVIN_ALLOW_ANY_HOST` / `KELVIN_ALLOWED_ORIGINS` | tunnel allowlisting | — |

`KELVIN_INDEX_DIR` deliberately defaults away from `web/public/kelvin`: those shards are the
deployed site's and are shared with Kirchhoff, and a rebuild triggered by whatever data dir this
server happens to point at would overwrite them. First run against a fresh index dir builds the
shards it needs (~16 s for the whole catalogue) and says so on stderr.

**Point `KELVIN_TAS_DATA_DIR` and `KELVIN_SHARD_DIR` at the same catalogue.** They are read by
two different engines — native for search/lookup/recommend, WASM for cross-reference — and if one
describes the public catalogue while the other describes the full TAS one, `search_parts` and
`cross_reference` will disagree about which parts exist, silently. The public site's data
(`web/public/kelvin` + the NDJSON it symlinks) is a consistent pair; so is
`PSMA/TAS/data` with shards built from it.

## Use it from Claude (web or Desktop)

Claude reaches your server over the public internet, so a localhost port needs a tunnel. Custom
connectors require a paid plan. Hertz uses 8400, Kirchhoff 8401 and Kelvin 8402, so all three can
run at once as separate connectors.

1. **Start the tunnel first** — you need its hostname before starting the server:

   ```bash
   npx cloudflared tunnel --url http://localhost:8402
   ```

2. **Start the server with that hostname allowed:**

   ```bash
   KELVIN_TAS_DATA_DIR=/path/to/TAS/data \
   KELVIN_PUBLIC_HOST=<random>.trycloudflare.com python3 mcp/server.py
   ```

   A pasted URL is fine — the scheme and path are stripped for you. `KELVIN_ALLOW_ANY_HOST=1`
   skips the check for throwaway tunnels (fine for a laptop, not for anything public).

   > **Why:** the MCP SDK enables DNS-rebinding protection by default and rejects unrecognised
   > `Host` headers with a bare `421 Invalid Host header`. Behind a tunnel the Host is the
   > *public* name, so every request 421s. Claude then cannot speak MCP, falls back to probing
   > for OAuth, and reports **"Couldn't register with <name>'s sign-in service"** — an
   > authentication error for what is actually a Host-header rejection. If you see that message,
   > `curl -i https://<tunnel>/mcp` before touching anything OAuth.

3. **Add the connector.** Claude → **Settings** → **Connectors** → **Add custom connector**,
   pasting the tunnel URL **with the `/mcp` path**. The bare hostname 404s, which produces the
   same misleading OAuth error. A connector added earlier serves a *cached* tool list — remove
   and re-add it after the tool surface changes.

## Tools

Seven. The catalogue has twelve families and dozens of query shapes; collapsing them into one
`search_parts` with a real filter language beats a tool per family (past ~25 tools, hosts start
picking the wrong one).

| Tool | Answers | Widget |
|---|---|---|
| `list_families` | what is in the catalogue at all | — |
| `describe_family` | the exact fields one family can be filtered/sorted/bucketed on | — |
| `search_parts` | "what parts do we have that …" — numeric ranges, categorical values, MPN, manufacturer, facet counts | picker |
| `part_details` | one part by MPN, with the full TAS record behind it | — |
| `recommend_parts` | "rank real parts for this requirement" — Kelvin's deterministic selector | picker |
| `spec_distribution` | "what is actually available" — histogram of one spec over a filtered slice | — |
| `cross_reference` | "what can replace this part" — scored substitutes with per-parameter verdicts | picker |

`describe_family` exists because the query vocabulary is per-family and generated from
`Browse.hpp`'s field table — so a caller learns the real field names instead of guessing, and an
unknown filter field is an error naming the alternatives, never a silently ignored filter.

Absence is never reported as a value: a part whose record does not state a field you filtered on
is excluded by that filter (a browse filter is a user filter, not the selector's rank-not-gate),
`spec_distribution` reports how many parts state the value at all, and a margin the record could
not state stays `null` rather than becoming a 0 that would read as the binding constraint.

## Where each answer comes from

```
list_families / describe_family / search_parts / part_details        PyKelvin  (native, in-process)
recommend_parts                                                      PyKelvin  Engine.select
cross_reference                                                      xref.mjs  (Node + the WASM build)
```

The split is deliberate. The substitute **ranker** is C++ (`CrossRef.hpp`) and PyKelvin exposes
it directly — but turning *"this MPN"* into a scored list also needs the per-family candidate
pre-gate and the shard-row → ranker-spec projection, and those live in `web/src/crossref.js`:
the module the site's CrossRefView renders from and the one Qarlos (the standing cross-reference
auditor) drives. A Python copy of that table would be a third copy, and copies drift — the file
says so itself. So `xref.mjs` is a thin long-lived worker that imports the real module and runs
it over the same `kelvin.js` WASM the browser loads, exactly as `tools/qarlos/probe.mjs` does;
only the transport differs (shards read off disk, not fetched). It starts on first use and
restarts if it dies.

Consequence worth knowing: `cross_reference` covers the seven families the ranker has a parameter
model for (`magnetic`, `capacitor`, `resistor`, `mosfet`, `timing`, `connector`, `diode`) and
refuses the rest by name. Search and lookup cover all twelve.

## Widgets

`ui://kelvin/picker.html` — sortable spec table, per-parameter pass/warn/fail pills, the ranker's
notes, margin chips, and a "use this" action that reports the choice back to the agent through
`updateModelContext`. Built from `src/picker.js` with `@modelcontextprotocol/ext-apps` and
bundled single-file by `vite-plugin-singlefile`, because MCP App resources render in a
deny-by-default CSP iframe.

One widget serves `search_parts`, `recommend_parts` and `cross_reference`: all three return the
same shape (a ranked list of candidates) and pose the same question (pick one), so they
normalise to one `candidates[]` envelope rather than three. The other four tools carry no
`ui/resourceUri` at all — advertising a UI a tool does not fill is how you get a broken panel.

`assert_widgets_resolve()` runs at startup and refuses to serve if a registered `ui://` has no
bundle behind it. That failure is not hypothetical: OpenMagnetics ships a curves widget URI with
no bundle and no build tooling in the repo, so eight sweep tools advertise a chart the host can
never fetch and nothing complained (ABT #651).

**ABT #663** asks `websharedcomponents` for a shared ranked-candidate picker covering Kirchhoff,
OpenMagnetics and Hertz as well. This widget is deliberately generic and deliberately disposable:
when that lands, this server should adopt it rather than keep a private copy.

## Testing

```bash
KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 mcp/smoke.py [--skip-xref]
```

Calls every tool against the real catalogue and asserts the answers are the catalogue's — that
gates hold, that an impossible requirement names the gate that rejected it, that a missing part
is refused rather than silently empty, that the cross-reference digest carries the ranker's
reasoning and not just names and scores, and that every advertised widget resolves. A broken tool
fails there, not in a chat session where "no candidates" and "nobody looked" read the same.

The engine underneath is covered by `test_kelvin` (Catch2); the browse vocabulary and the
record-span fetch this server depends on are in `[browse]`.

## Status

Spike. Not done: auth (bind to loopback and put a reverse proxy in front if it is ever exposed),
a histogram widget for `spec_distribution`, and a shard-side MPN index — `part_details` and
`cross_reference` resolve an MPN by scanning the shard, which is keyed for selection, not lookup.

Known catalogue gap, not a server gap: there is **no safety-class or AC-voltage field** anywhere
in the capacitor records, so "X2 rated 310 VAC" cannot be expressed as a filter — the X1/X2
distinction only exists as prose in `part.series` / `manufacturerInfo.family`. That is the same
gap behind ABT #557 (an X1 original cross-referenced to X2 parts and graded a clean upgrade).
