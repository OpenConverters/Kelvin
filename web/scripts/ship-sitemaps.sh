#!/usr/bin/env bash
# Copy the sitemaps built by build-sitemaps.sh to the box and prove they are served.
#
#   web/scripts/ship-sitemaps.sh [SITEMAP_DIR]        # default web/.sitemaps
#
# The service refuses (503) a sitemap built from a different catalogue than the one it serves,
# so a bad ship is caught by the first request below — and the previous sitemaps are put back.
# Then a spread of the listed URLs is fetched: every one must be a real part page (200).
# Called at the end of deploy-kelvin-data.sh; safe to run on its own after a data deploy.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(cd "${1:-$HERE/.sitemaps}" && pwd)"
HOST=root@51.15.253.66
SSH="ssh -i $HOME/.ssh/om_scaleway -o StrictHostKeyChecking=no"
REMOTE=/cache/kelvin-mcp-public/sitemaps
BASE=https://kelvin.openconverters.com
SAMPLE="${SAMPLE:-60}"

[[ -f "$DIR/build.json" && -f "$DIR/sitemap.xml" ]] || { echo "no finished sitemap build in $DIR" >&2; exit 1; }

echo "shipping $(ls "$DIR" | wc -l) sitemap files ($(du -sh "$DIR" | cut -f1)) -> $REMOTE"
rsync -az --delete -e "$SSH" "$DIR/" "$HOST:$REMOTE.new/" --rsync-path="mkdir -p $REMOTE.new && rsync"
$SSH "$HOST" "set -e; rm -rf $REMOTE.old; [ -d $REMOTE ] && mv $REMOTE $REMOTE.old; mv $REMOTE.new $REMOTE"

restore() {
  echo "ROLLING BACK the sitemaps" >&2
  $SSH "$HOST" "set -e; if [ -d $REMOTE.old ]; then rm -rf $REMOTE; mv $REMOTE.old $REMOTE; else rm -rf $REMOTE; fi"
  exit 1
}

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/sitemap.xml")
if [[ "$code" != 200 ]]; then
  echo "live /sitemap.xml answered $code (503 = built from a different catalogue than the box serves)" >&2
  restore
fi

# An evenly spread sample of every child file, paced under nginx's 40 requests/minute.
bad=0; n=0
mapfile -t URLS < <(python3 - "$DIR" "$SAMPLE" <<'PY'
import glob, re, sys
files = sorted(glob.glob(f"{sys.argv[1]}/sitemap-*-*.xml"))
per = max(1, int(sys.argv[2]) // len(files))
for f in files:
    urls = [u.replace("&amp;", "&") for u in re.findall(r"<loc>([^<]+)</loc>", open(f).read())]
    step = max(1, len(urls) // per)
    print("\n".join(urls[::step][:per]))
PY
)
for url in "${URLS[@]}"; do
  n=$((n + 1))
  code=$(curl -s -o /dev/null -w '%{http_code}' "$url")
  [[ "$code" == 429 ]] && { sleep 5; code=$(curl -s -o /dev/null -w '%{http_code}' "$url"); }
  if [[ "$code" != 200 ]]; then echo "  $code  $url" >&2; bad=$((bad + 1)); fi
  sleep 1.6
done
if (( bad )); then echo "$bad of $n sampled sitemap URLs are not served" >&2; restore; fi
$SSH "$HOST" "rm -rf $REMOTE.old"
echo "sitemaps live and verified ($n sampled URLs all 200)"
