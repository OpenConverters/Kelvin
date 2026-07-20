#!/usr/bin/env bash
# Deploy the Kelvin DATA set (shards + NDJSON + manifest) to /cache/kelvin on the
# Scaleway box, which BOTH kelvin.openconverters.com and kirchhoff.openconverters.com
# read from. The SPA deploy scripts deliberately do not touch this path; this is it.
#
#   web/scripts/deploy-kelvin-data.sh                  # all families
#   web/scripts/deploy-kelvin-data.sh magnetic diode   # only these families
#
# Why this script exists (2026-07-20): the data set was previously deployed by hand.
# Two things went wrong doing it that way, and both are automated here:
#
#  1. A fabricated-parts sweep can be forgotten. The guard runs first, and a failure
#     aborts the deploy — invented parts must never reach a served catalogue.
#
#  2. STALE .gz SIDECARS. nginx has gzip_static on, so it serves <file>.gz in
#     preference to <file> for any client sending Accept-Encoding: gzip — i.e. every
#     browser. Replacing manifest.json without regenerating manifest.json.gz means
#     curl sees the new data and every real user sees the OLD data. That happened:
#     the corrected catalogue was live but browsers kept loading the previous
#     manifest, so the fabricated parts appeared to still be there. Sidecars are now
#     regenerated for every file shipped, always.
#
# The shard, its NDJSON and the manifest MUST move together — manifest.sourceSize is
# what the client checks before trusting a byte offset — so they are staged and then
# swapped, never rsynced piecemeal into the live directory.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"          # web/
SRC="$HERE/public/kelvin"
HOST=root@51.15.253.66
SSH="ssh -i $HOME/.ssh/om_scaleway -o StrictHostKeyChecking=no"
REMOTE=/cache/kelvin
STAGE="$REMOTE/.staging-$$"
TAS_DATA="${TAS_DATA:-/home/alf/PSMA/TAS/data}"
FAB_GUARD="${FAB_GUARD:-$(dirname "$TAS_DATA")/scripts/check_no_fabricated_parts.py}"

FAMILIES=("$@")
if [[ ${#FAMILIES[@]} -eq 0 ]]; then
  FAMILIES=(mosfet diode capacitor resistor controller igbt bjt varistor analog timing connector magnetic)
fi

# 1. Fabrication guard — fail closed.
[[ -f "$FAB_GUARD" ]] || { echo "REFUSING: fabrication guard missing at $FAB_GUARD" >&2; exit 1; }
python3 "$FAB_GUARD" --data "$TAS_DATA" || { echo "REFUSING to deploy fabricated parts." >&2; exit 1; }

# 2. Local set must be self-consistent: every shard's manifest.sourceSize must equal
#    the NDJSON it will ship next to, or the client's version guard will reject reads.
python3 - "$SRC" "${FAMILIES[@]}" <<'PY'
import json, os, sys
src, families = sys.argv[1], sys.argv[2:]
manifest = json.load(open(os.path.join(src, "manifest.json")))
bad = []
for family in families:
    entry = manifest["families"].get(family)
    if entry is None:
        bad.append(f"{family}: absent from manifest.json"); continue
    ndjson = os.path.realpath(os.path.join(src, f"{family}.ndjson"))
    if not os.path.exists(ndjson):
        bad.append(f"{family}: {ndjson} missing"); continue
    size = os.path.getsize(ndjson)
    if size != entry["sourceSize"]:
        bad.append(f"{family}: ndjson {size} B != manifest sourceSize {entry['sourceSize']} B "
                   "(rebuild shards — they were indexed against a different catalogue)")
if bad:
    sys.exit("REFUSING to deploy:\n  " + "\n  ".join(bad))
print(f"local set consistent for: {', '.join(families)}")
PY

# 3. Stage (‑‑copy-links: public/kelvin/*.ndjson are symlinks into the TAS checkout).
echo "staging ${#FAMILIES[@]} families -> $STAGE"
FILES=(manifest.json)
for family in "${FAMILIES[@]}"; do FILES+=("$family.kidx" "$family.ndjson"); done
(cd "$SRC" && rsync -az --copy-links -e "$SSH" "${FILES[@]}" \
  "$HOST:$STAGE/" --rsync-path="mkdir -p $STAGE && rsync")

# 4. Swap in, then regenerate EVERY shipped file's .gz sidecar (see header note 2).
$SSH "$HOST" "set -e
  cd '$STAGE'
  for f in *; do mv -f \"\$f\" '$REMOTE'/\"\$f\"; done
  cd '$REMOTE'
  for f in ${FILES[*]}; do
    case \"\$f\" in *.ndjson) continue;; esac   # NDJSON is Range-served, never gzip_static
    gzip -kf9 \"\$f\"
  done
  chown alf:sudo ${FILES[*]} manifest.json.gz 2>/dev/null || true
  rmdir '$STAGE'"

# 5. Verify what browsers actually receive (--compressed => the .gz path).
echo "verifying live…"
for base in https://kelvin.openconverters.com https://kirchhoff.openconverters.com; do
  curl -sf --compressed "$base/kelvin/manifest.json" -o /tmp/kelvin-live-manifest.$$ || {
    echo "  $base: manifest not reachable" >&2; exit 1; }
  python3 - "$SRC/manifest.json" /tmp/kelvin-live-manifest.$$ "$base" "${FAMILIES[@]}" <<'PY'
import json, sys
local = json.load(open(sys.argv[1]))["families"]
live = json.load(open(sys.argv[2]))["families"]
base, families = sys.argv[3], sys.argv[4:]
bad = [f"{f}: live buildId {live.get(f,{}).get('buildId')} != local {local[f]['buildId']}"
       for f in families if live.get(f, {}).get("buildId") != local[f]["buildId"]]
if bad:
    sys.exit(f"STALE at {base} (a .gz sidecar is almost certainly the cause):\n  " + "\n  ".join(bad))
print(f"  {base}: serving the deployed buildIds")
PY
  rm -f /tmp/kelvin-live-manifest.$$
done
echo "Kelvin data deploy verified."
