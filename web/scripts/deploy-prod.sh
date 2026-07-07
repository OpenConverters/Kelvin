#!/usr/bin/env bash
# Deploy the Kelvin site to kelvin.openconverters.com (Scaleway box, same host as
# kirchhoff.openconverters.com). Idempotent — safe to re-run for every release.
#
#   web/scripts/deploy-prod.sh            # build + rsync + sidecars + vhost + verify
#   SKIP_NGINX=1 web/scripts/deploy-prod.sh   # SPA-only redeploy (vhost untouched)
#
# Data note: the SPA reads /kelvin/{manifest.json,*.kidx,*.ndjson} from /cache/kelvin
# on the box — the SAME self-consistent set kirchhoff.openconverters.com serves. This
# script does NOT touch it; refresh it with the existing Kirchhoff data flow (shards +
# NDJSON + manifest together, then regenerate their .gz sidecars).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"     # web/
HOST=root@51.15.253.66
SSH="ssh -i $HOME/.ssh/om_scaleway -o StrictHostKeyChecking=no"
DOCROOT=/opt/kelvin/dist

# 1. Clean-HEAD build (refuse to ship uncommitted work — the byte-verify rule).
if [[ -n "$(git -C "$HERE" status --porcelain -- . 2>/dev/null)" ]]; then
  echo "REFUSING to deploy: web/ has uncommitted changes (a build bundles the working tree)." >&2
  exit 1
fi
( cd "$HERE" && npm run build )

# 2. SPA rsync (the /kelvin data path is served off /cache, never from the docroot).
$SSH "$HOST" "mkdir -p $DOCROOT"
rsync -az --delete --exclude 'kelvin/' -e "$SSH" "$HERE/dist/" "$HOST:$DOCROOT/"

# 3. gzip sidecars (gzip_static on — a stale .gz silently serves old bytes to browsers).
$SSH "$HOST" "cd $DOCROOT && find . -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' \) -not -name '*.gz' -exec gzip -kf9 {} \;"

# 4. nginx vhost + TLS (first run only; reuses Kirchhoff's conventions).
if [[ -z "${SKIP_NGINX:-}" ]]; then
  scp -i "$HOME/.ssh/om_scaleway" -o StrictHostKeyChecking=no "$HERE/scripts/nginx-kelvin.conf" "$HOST:/etc/nginx/sites-available/kelvin"
  $SSH "$HOST" "ln -sfn /etc/nginx/sites-available/kelvin /etc/nginx/sites-enabled/kelvin && nginx -t && systemctl reload nginx"
  # cert (no-op if it already exists); certbot rewrites the vhost for 443 + redirect
  $SSH "$HOST" "certbot --nginx -d kelvin.openconverters.com --non-interactive --agree-tos -m openmagnetics@protonmail.com && nginx -t && systemctl reload nginx"
fi

# 5. Byte-verify the live artifacts against this clean-HEAD build (per artifact).
BASE=https://kelvin.openconverters.com
if [[ -n "${SKIP_NGINX:-}" ]] || curl -sfI "$BASE/" >/dev/null 2>&1; then
  for f in kelvin.js index.html $(cd "$HERE/dist" && ls assets/*.js assets/*.css); do
    live=$(curl -sf "$BASE/$f" | sha256sum | cut -d' ' -f1)
    local_=$(sha256sum "$HERE/dist/$f" | cut -d' ' -f1)
    if [[ "$live" != "$local_" ]]; then
      echo "BYTE MISMATCH on $f (live $live vs built $local_) — stale .gz sidecar or stray tree change" >&2
      exit 1
    fi
    echo "verified $f"
  done
  # Range sanity: the drawer refuses anything but a 206.
  code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=0-99' "$BASE/kelvin/mosfet.ndjson")
  [[ "$code" == 206 ]] || { echo "Range check failed: /kelvin/mosfet.ndjson returned $code (need 206)" >&2; exit 1; }
  echo "Range 206 OK — deploy verified."
else
  echo "https not answering yet (first deploy: run the nginx/certbot step)." >&2
fi
