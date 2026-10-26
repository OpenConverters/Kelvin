#!/bin/sh
# Rebuild the sitemaps for the public Kelvin MCP and restart it. Run ON THE BOX after the
# catalogue under /cache/kelvin-tas or the shards under /cache/kelvin change (a data deploy):
# the sitemaps name exactly the parts the deployed engine can serve, and the service refuses
# (503) to serve a sitemap built from a different catalogue.
#
# One family per process, each under a hard memory cap, because the box is shared: loading the
# capacitor shard alone takes ~2 GB resident, and on a host already in swap an uncapped build
# would starve its neighbours. A family that does not fit the cap fails ITS build (the cgroup
# kills only that process) and the script stops, leaving the old sitemaps in place; raise
# CAP only after checking `free -m` shows that much available.
#
#   ssh root@<box> /cache/kelvin-mcp-public/mcp/../kelvin-sitemaps.sh      (or run from web/scripts)
set -eu
CAP=${CAP:-2300M}
unit=/etc/systemd/system/kelvin-mcp-public.service
env_of() { sed -n "s/^Environment=$1=//p" "$unit"; }
OUT=$(env_of KELVIN_SITEMAP_DIR)
PY=/cache/moebius/venv/bin/python
CODE=/cache/kelvin-mcp-public/mcp
run() {
  systemd-run --wait --collect --quiet -p MemoryMax="$CAP" -p MemorySwapMax=0 -p Nice=15 \
    -p WorkingDirectory="$CODE" \
    -E KELVIN_TAS_DATA_DIR="$(env_of KELVIN_TAS_DATA_DIR)" -E KELVIN_SHARD_DIR="$(env_of KELVIN_SHARD_DIR)" \
    -E KELVIN_INDEX_DIR="$(env_of KELVIN_INDEX_DIR)" -E KELVIN_PUBLIC_HOST="$(env_of KELVIN_PUBLIC_HOST)" \
    -E KELVIN_PUBLIC_MODE=1 "$PY" "$CODE/sitemaps.py" "$@"
}
STAGE="$OUT.new"; rm -rf "$STAGE"; mkdir -p "$STAGE"
for f in $(run --list-families | grep -E '^[a-z]+$'); do run "$STAGE" --family "$f"; done
run "$STAGE" --finish
rm -rf "$OUT.old"; [ -d "$OUT" ] && mv "$OUT" "$OUT.old"; mv "$STAGE" "$OUT"; rm -rf "$OUT.old"
echo "sitemaps live in $OUT"
