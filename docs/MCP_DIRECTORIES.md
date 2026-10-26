# Listing Kelvin in MCP directories

Facts every form asks for:

- **Name:** Kelvin — power electronics components
- **Registry name:** `io.github.OpenConverters/kelvin` (official MCP registry, published 0.1.0)
- **Endpoint:** `https://kelvin.openconverters.com/mcp` — Streamable HTTP, no authentication
- **Repo:** https://github.com/OpenConverters/Kelvin  · **Site:** https://kelvin.openconverters.com
- **One line:** Read-only, deterministic parts catalogue for power electronics (~1.4 M parts): parametric search, part lookup, requirement-based selection, scored cross-reference, spec distributions. No LLM inside.
- **Tools:** list_families, describe_family, search_parts, part_details, recommend_parts, cross_reference, spec_distribution  · **Prompts:** find_replacement, shortlist_parts, spec_landscape
- **Categories / tags:** electronics, power electronics, components, parts database, datasheet, BOM, cross-reference, MOSFET, capacitor, inductor
- **Machine-readable:** `/llms.txt`, `/.well-known/mcp/server-card.json`

| Directory | How it lists | Status |
|---|---|---|
| Official MCP registry | `mcp-publisher publish` (org namespace needs an Owner PAT with `read:org`) | **published** |
| PulseMCP | ingests the official registry automatically | nothing to do; check pulsemcp.com/servers after the next weekly crawl |
| Glama | indexes public GitHub repos; `glama.json` in the repo root names the maintainer for claiming | file added; claim at glama.ai/mcp/servers once indexed |
| Smithery | smithery.ai/new → "URL" (external remote server), GitHub login | needs the owner's login |
| mcp.so | mcp.so/submit form, GitHub login | needs the owner's login |
| awesome-mcp-servers (punkpeanut) | pull request adding a line under the electronics / hardware section | needs a GitHub login (`gh auth login`) |
| MCP Market, Cursor Directory, others | web forms with the facts above | needs the owner's login |

PR line for awesome-mcp-servers:

```
- [OpenConverters/Kelvin](https://github.com/OpenConverters/Kelvin) 🐍 ☁️ - Read-only parts catalogue for power electronics (~1.4 M MOSFETs, diodes, capacitors, resistors, magnetics, controllers…): parametric search, selection, scored cross-reference. Remote: https://kelvin.openconverters.com/mcp
```
