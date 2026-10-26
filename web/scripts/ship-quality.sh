#!/usr/bin/env bash
# Build quality.json from the latest metrics run and publish it to the box (ABT #1924).
#   web/scripts/ship-quality.sh        # needs TAS/docs/metrics/history.ndjson from a recent run
# Refuses a stale metrics run (build-quality.py). Called by deploy-kelvin-data.sh (SKIP_QUALITY=1).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
HOST=root@51.15.253.66
SSH="ssh -i $HOME/.ssh/om_scaleway -o StrictHostKeyChecking=no"
python3 "$HERE/scripts/build-quality.py" "$HERE/.quality/quality.json"
scp -q -i "$HOME/.ssh/om_scaleway" -o StrictHostKeyChecking=no "$HERE/.quality/quality.json" "$HOST:/cache/kelvin-mcp-public/quality.json.new"
$SSH "$HOST" "mv /cache/kelvin-mcp-public/quality.json.new /cache/kelvin-mcp-public/quality.json"
live=$(curl -sf https://kelvin.openconverters.com/api/v1/quality | python3 -c "import sys,json;print(json.load(sys.stdin)['date'])")
local=$(python3 -c "import json;print(json.load(open('$HERE/.quality/quality.json'))['date'])")
[[ "$live" == "$local" ]] || { echo "live quality date $live != $local" >&2; exit 1; }
echo "quality page live (metrics run $live)"
