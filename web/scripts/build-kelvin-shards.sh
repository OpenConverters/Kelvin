#!/usr/bin/env bash
# Build the per-family index shards + manifest the Kelvin site serves from public/kelvin/.
# Build artifacts (gitignored); regenerate whenever the TAS DB changes.
#
#   KELVIN_INDEX=/path/to/Kelvin/build/kelvin-index \
#   TAS_DATA=/path/to/TAS/data \
#   MAGNETICS_DATA=/path/to/dir-with-magnetics.ndjson \
#   web/scripts/build-kelvin-shards.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"                       # web/
KELVIN_INDEX="${KELVIN_INDEX:-$HERE/../build/kelvin-index}"
TAS_DATA="${TAS_DATA:-/home/alf/PSMA/TAS/data}"
MAGNETICS_DATA="${MAGNETICS_DATA:-/home/alf/OpenConverters/Heaviside/TAS/data}"
OUT="$HERE/public/kelvin"

if [[ ! -x "$KELVIN_INDEX" ]]; then
  echo "kelvin-index not found at $KELVIN_INDEX — build Kelvin (ninja -C build) or set KELVIN_INDEX" >&2
  exit 1
fi

# GUARD: refuse to index a catalogue containing fabricated (script-synthesized) parts.
# A user found 177 invented Würth/Coilcraft/TDK magnetics on the live site in July 2026 —
# they had been indexed and shipped because nothing stood between the data and the shard.
# This is that something. Never make it non-fatal; a fabricated part must not be indexable.
FAB_GUARD="${FAB_GUARD:-$(dirname "$TAS_DATA")/scripts/check_no_fabricated_parts.py}"
if [[ -f "$FAB_GUARD" ]]; then
  echo "checking catalogues for fabricated parts…"
  python3 "$FAB_GUARD" --data "$TAS_DATA" || {
    echo "REFUSING to build shards: fabricated parts in $TAS_DATA (see above)." >&2; exit 1; }
  if [[ "$MAGNETICS_DATA" != "$TAS_DATA" ]]; then
    python3 "$FAB_GUARD" --data "$MAGNETICS_DATA" || {
      echo "REFUSING to build shards: fabricated parts in $MAGNETICS_DATA (see above)." >&2; exit 1; }
  fi
else
  echo "REFUSING to build shards: fabrication guard not found at $FAB_GUARD" >&2
  exit 1
fi

mkdir -p "$OUT"

# ten families from the TAS data dir; magnetics live in their own dir
for fam in mosfet diode capacitor resistor controller igbt bjt varistor analog timing connector; do
  "$KELVIN_INDEX" --data "$TAS_DATA" --out "$OUT" --family "$fam"
done
"$KELVIN_INDEX" --data "$MAGNETICS_DATA" --out "$OUT" --family magnetic

# Host each catalog's NDJSON next to its shard so the drawer can Range-fetch ONE
# record (bytes=srcOffset-…) on open. Symlinks keep dev disk ~0; the prod deploy
# rsync -L resolves them into real files. Bytes MUST stay identical to what the
# shard was indexed from (manifest.sourceSize guards this at fetch time).
for fam in mosfet diode capacitor resistor controller igbt bjt varistor; do
  src="$TAS_DATA/${fam}s.ndjson"
  if [[ -f "$src" ]]; then
    ln -sfn "$src" "$OUT/${fam}.ndjson"
  else
    echo "WARN: source catalog $src missing — record fetch for '$fam' will 404 until hosted" >&2
  fi
done
ln -sfn "$MAGNETICS_DATA/magnetics.ndjson" "$OUT/magnetic.ndjson"
ln -sfn "$TAS_DATA/analog_ics.ndjson" "$OUT/analog.ndjson"
ln -sfn "$TAS_DATA/timing_devices.ndjson" "$OUT/timing.ndjson"
ln -sfn "$TAS_DATA/connectors.ndjson" "$OUT/connector.ndjson"

echo "Kelvin shards + NDJSON written to $OUT"
ls -la "$OUT"
