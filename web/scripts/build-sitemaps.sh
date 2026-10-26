#!/usr/bin/env bash
# Build the sitemaps for the PUBLIC Kelvin part pages from the data set that is being (or has
# just been) deployed — never from the master TAS data, which holds parts and fields the public
# set does not (ABT #456) and so would list pages the site cannot serve.
#
#   web/scripts/build-sitemaps.sh [DATA_SRC] [OUT]
#     DATA_SRC  dir holding <family>.ndjson + manifest.json   (default web/public/kelvin)
#     OUT       output dir                                    (default web/.sitemaps)
#
# The server names its catalogue files by TAS name (capacitors.ndjson, ...) and hashes those
# names and sizes into the data version the sitemap must match, so the files are exposed under
# the same names the box uses (/cache/kelvin-tas). Runs in a throwaway index dir: it must not
# touch ~/.kelvin/index or the shards under web/public/kelvin.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"          # web/
SRC="$(cd "${1:-$HERE/public/kelvin}" && pwd)"
OUT="${2:-$HERE/.sitemaps}"
declare -A TAS=( [analog]=analog_ics [bjt]=bjts [capacitor]=capacitors [connector]=connectors
  [controller]=controllers [diode]=diodes [igbt]=igbts [magnetic]=magnetics [mosfet]=mosfets
  [resistor]=resistors [timing]=timing_devices [varistor]=varistors )

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/data" "$WORK/index"
for family in "${!TAS[@]}"; do
  [[ -e "$SRC/$family.ndjson" ]] || { echo "REFUSING: $SRC/$family.ndjson missing or a dead link" >&2; exit 1; }
  ln -s "$(readlink -f "$SRC/$family.ndjson")" "$WORK/data/${TAS[$family]}.ndjson"
done
[[ -f "$SRC/manifest.json" ]] || { echo "REFUSING: $SRC/manifest.json missing" >&2; exit 1; }

rm -rf "$OUT"
KELVIN_TAS_DATA_DIR="$WORK/data" KELVIN_SHARD_DIR="$SRC" KELVIN_INDEX_DIR="$WORK/index" \
KELVIN_PUBLIC_HOST=kelvin.openconverters.com \
  python3 "$HERE/../mcp/sitemaps.py" "$OUT"
