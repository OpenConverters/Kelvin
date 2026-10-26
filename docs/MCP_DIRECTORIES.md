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
| Glama | auto-ingested from the official registry | **listed:** https://glama.ai/mcp/connectors/io.github.OpenConverters/kelvin (scored badge live); `glama.json` names the maintainer for claiming |
| Smithery | smithery.ai/new → "URL" (external remote server), GitHub login | needs the owner's login |
| mcp.so | mcp.so/submit form, GitHub login | needs the owner's login |
| awesome-remote-mcp-servers (punkpeye) | pull request, Science & Research; requires a Glama connector badge (Glama ingested us from the official registry) | **PR open: punkpeye/awesome-remote-mcp-servers#3021** |
| awesome-mcp-servers (punkpeye) | out of scope: that list is for installable servers, remote-only ones go to the list above | not applicable |
| MCP Market, Cursor Directory, others | web forms with the facts above | needs the owner's login |
