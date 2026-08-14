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

# --check exits after the guards, before anything is staged or swapped. Added because the
# lockstep guard below has an override, and the only way to exercise an override used to be to
# run the real deploy with it set — which on 2026-08-14 shipped a v11 capacitor shard onto a
# v10 prod engine and broke live capacitor browsing until it was rolled back. A safety check
# nobody can rehearse without firing the thing it guards is not a safety check.
DRY_RUN=""
if [[ "${1:-}" == "--check" ]]; then DRY_RUN=1; shift; fi

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

# 2b. SHARD FORMAT LOCKSTEP. /cache/kelvin is read by TWO independently deployed engines —
#     kelvin.openconverters.com's kelvin.js and kirchhoff.openconverters.com's, which embeds
#     its own copy of the same C++ via deps/Kelvin. The shard header carries a format version
#     and the reader demands an EXACT match, throwing "unsupported shard format version"
#     rather than degrading. So a format bump cannot be deployed one artifact at a time: ship
#     new shards to old engines and both sites break; ship a new engine against live old
#     shards and that site breaks. There is no safe order, only a coordinated swap.
#
#     This is not hypothetical. On 2026-08-14 Kelvin's engine had been on v11 for two days
#     while Kirchhoff's embedded copy was still v10 and prod served v10 shards — a live
#     mismatch waiting for whoever deployed next. Nothing in this script noticed, because it
#     only ever checked the data against itself.
#
#     So: refuse unless the shards being shipped, this checkout's engine, and Kirchhoff's
#     embedded engine all agree — and refuse again if that version differs from what is
#     LIVE, because that is the coordinated-swap case and it needs a human who knows both
#     SPAs are going out with it.
KELVIN_SRC_INDEX="$HERE/../src/Index.cpp"
KIRCHHOFF_INDEX="${KIRCHHOFF_DIR:-$HERE/../../Kirchhoff}/deps/Kelvin/src/Index.cpp"
LIVE_VER="$($SSH "$HOST" "od -An -tu4 -j8 -N4 $REMOTE/manifest.json >/dev/null 2>&1; \
  for f in $REMOTE/*.kidx; do od -An -tu4 -j8 -N4 \"\$f\"; break; done" 2>/dev/null | tr -d ' ' || true)"
python3 - "$SRC" "$KELVIN_SRC_INDEX" "$KIRCHHOFF_INDEX" "${LIVE_VER:-none}" \
         "${KELVIN_FORMAT_BUMP:-}" "${FAMILIES[@]}" <<'PY'
import os, re, struct, sys
src, kelvin_index, kirchhoff_index, live, acknowledged, families = (
    sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6:])

def engine_version(path, label):
    if not os.path.exists(path):
        sys.exit(f"REFUSING to deploy: cannot read {label}'s shard format from {path}. "
                 "Both engines must be verifiable before shards move; set KIRCHHOFF_DIR "
                 "if that checkout lives elsewhere.")
    m = re.search(r'kFormatVersion\s*=\s*(\d+)', open(path).read())
    if not m:
        sys.exit(f"REFUSING to deploy: no kFormatVersion in {path} ({label}).")
    return int(m.group(1))

shard = {}
for family in families:
    p = os.path.join(src, f"{family}.kidx")
    with open(p, "rb") as fh:
        head = fh.read(12)
    shard[family] = struct.unpack("<I", head[8:12])[0]
versions = set(shard.values())
if len(versions) != 1:
    sys.exit("REFUSING to deploy: the shards being shipped are not one format — "
             + ", ".join(f"{k}=v{v}" for k, v in sorted(shard.items())))
ship = versions.pop()

kelvin_v = engine_version(kelvin_index, "kelvin.openconverters.com")
kirch_v = engine_version(kirchhoff_index, "kirchhoff.openconverters.com")
if not (ship == kelvin_v == kirch_v):
    sys.exit(f"REFUSING to deploy: shard format v{ship} but Kelvin's engine reads v{kelvin_v} "
             f"and Kirchhoff's embedded engine reads v{kirch_v}. Both sites read these files "
             "and the reader is an exact-match check. Advance the lagging engine "
             "(Kirchhoff: deps/Kelvin submodule) and rebuild before shipping data.")

if live not in ("none", "") and int(live) != ship and not acknowledged:
    sys.exit(
        f"REFUSING to deploy: this is a FORMAT BUMP — live shards are v{live}, these are "
        f"v{ship}. Whichever artifact lands first, the other site is broken until its engine "
        "follows, so the SPAs must go out with the data rather than after it. Deploy both "
        "engines in the same window, then re-run with KELVIN_FORMAT_BUMP=1 to acknowledge.")
print(f"shard format lockstep ok: shards v{ship}, both engines v{ship}, live v{live}")
PY
if [[ -n "${KELVIN_FORMAT_BUMP:-}" ]]; then
  echo "KELVIN_FORMAT_BUMP set — proceeding with a coordinated format swap."
fi

if [[ -n "$DRY_RUN" ]]; then
  echo "--check: guards passed, nothing staged or deployed."
  exit 0
fi

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
