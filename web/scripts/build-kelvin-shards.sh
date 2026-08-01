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

# PUBLIC STRIP (ABT #456, 2026-08-01): the served data set is a REDACTED variant of
# the master catalogs — verbatim vendor descriptions and named source endpoints
# (provenance sourceName/sourceUrl) are removed by strip-public-fields.py; the
# coarse provenance kind and all parametric facts stay. The master TAS data keeps
# full attribution. NEVER index or symlink the raw TAS files here again: shipping
# 377k vendor marketing strings as one downloadable file is a takedown-class
# exposure, and this build step is what stands between the master and the site.
PUBLIC_DATA="${PUBLIC_DATA:-$HOME/kelvin-public-data}"
STRIP="$HERE/scripts/strip-public-fields.py"
[[ -f "$STRIP" ]] || { echo "REFUSING: $STRIP missing — cannot build the public data set" >&2; exit 1; }
mkdir -p "$PUBLIC_DATA"
for src_name in mosfets diodes capacitors resistors controllers igbts bjts varistors analog_ics timing_devices connectors; do
  src="$TAS_DATA/${src_name}.ndjson"
  [[ -f "$src" ]] || { echo "WARN: $src missing — skipped" >&2; continue; }
  python3 "$STRIP" "$src" "$PUBLIC_DATA/${src_name}.ndjson"
done
python3 "$STRIP" "$MAGNETICS_DATA/magnetics.ndjson" "$PUBLIC_DATA/magnetics.ndjson"

# twelve families, ALL indexed from the stripped public copies (byte offsets in the
# shards must match the file that will be served, never the master)
for fam in mosfet diode capacitor resistor controller igbt bjt varistor analog timing connector magnetic; do
  "$KELVIN_INDEX" --data "$PUBLIC_DATA" --out "$OUT" --family "$fam"
done

# Host each catalog's NDJSON next to its shard so the drawer can Range-fetch ONE
# record (bytes=srcOffset-…) on open. Symlinks keep dev disk ~0; the prod deploy
# rsync -L resolves them into real files. Bytes MUST stay identical to what the
# shard was indexed from (manifest.sourceSize guards this at fetch time).
for fam in mosfet diode capacitor resistor controller igbt bjt varistor; do
  src="$PUBLIC_DATA/${fam}s.ndjson"
  if [[ -f "$src" ]]; then
    ln -sfn "$src" "$OUT/${fam}.ndjson"
  else
    echo "WARN: public catalog $src missing — record fetch for '$fam' will 404 until hosted" >&2
  fi
done
ln -sfn "$PUBLIC_DATA/magnetics.ndjson" "$OUT/magnetic.ndjson"
ln -sfn "$PUBLIC_DATA/analog_ics.ndjson" "$OUT/analog.ndjson"
ln -sfn "$PUBLIC_DATA/timing_devices.ndjson" "$OUT/timing.ndjson"
ln -sfn "$PUBLIC_DATA/connectors.ndjson" "$OUT/connector.ndjson"

# Post-strip guard: the about-to-be-served set must contain no verbatim descriptions
# and no named source endpoints. Fail closed.
python3 - "$PUBLIC_DATA" <<'PY'
import glob, sys
bad = []
for f in glob.glob(sys.argv[1] + "/*.ndjson"):
    for token in ('"sourceName"', '"sourceUrl"'):
        with open(f, 'rb') as fh:
            for i, line in enumerate(fh, 1):
                if token.encode() in line:
                    bad.append(f"{f}:{i}: contains {token}")
                    break
if bad:
    sys.exit("REFUSING: public data set is not stripped:\n  " + "\n  ".join(bad))
print("public strip guard: clean")
PY

echo "Kelvin shards + NDJSON written to $OUT"
ls -la "$OUT"
