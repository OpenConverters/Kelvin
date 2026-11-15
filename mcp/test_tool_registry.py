"""Every public tool is registered under its own name.

A misplaced @mcp.tool decorator (on the helper _watts instead of recommend_parts, f9443ac)
silently renamed the recommender to '_watts' on the public MCP; tests that call the Python
functions directly cannot see that. This reads the tool list the server actually serves."""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

EXPECTED = {
    "list_families", "describe_family", "search_parts", "part_details", "recommend_parts",
    "spec_distribution", "cross_reference", "job_status", "job_result", "crossref_bom",
    "crossref_bom_line", "crossref_bom_lines",
}


def test_tool_names_are_the_public_functions():
    import server
    names = {t.name for t in asyncio.run(server.mcp.list_tools())}
    assert not [n for n in names if n.startswith("_")], f"private helper registered as a tool: {names}"
    assert names == EXPECTED, f"missing {EXPECTED - names}, unexpected {names - EXPECTED}"
