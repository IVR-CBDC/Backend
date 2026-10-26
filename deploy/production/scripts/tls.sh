#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=common.sh
source "$HERE/common.sh"
mode="${1:-}"
[[ "$mode" == --issue || "$mode" == --renew ]] || fail 'Usage: tls.sh --issue EMAIL --accept-terms | tls.sh --renew'
if [[ "$mode" == --issue ]]; then
    [[ $# == 3 && "$3" == --accept-terms && "$2" == *@* ]] || fail 'Explicit email and --accept-terms required'
fi
lock_deploy
load_release
[[ "$mode" != --renew || -f "$SHARED/tls-enabled" ]] || fail 'TLS has not been enabled'
space_guard $((1024*1024*1024))
"${DC[@]}" --profile tools pull certbot
if [[ "$mode" == --issue ]]; then
    "${DC[@]}" --profile tools run --rm --no-deps -T certbot certonly --non-interactive \
        --webroot -w /var/www/acme --domain alfacbcd.ru --cert-name alfacbcd.ru --email "$2" --agree-tos --no-eff-email
    cp "$RELEASE/proxy/https.conf" "$SHARED/proxy/default.conf"
    chmod 644 "$SHARED/proxy/default.conf"
else
    "${DC[@]}" --profile tools run --rm --no-deps -T certbot renew --non-interactive --quiet
fi
# No Docker socket in certbot, renewal reload is done here by deploy's cron.
if ! "${DC[@]}" exec -T proxy nginx -t; then
    if [[ "$mode" == --issue ]]; then
        cp "$RELEASE/proxy/http.conf" "$SHARED/proxy/default.conf"
        chmod 644 "$SHARED/proxy/default.conf"
    fi
    fail 'nginx validation failed; certificate files preserved'
fi
"${DC[@]}" exec -T proxy nginx -s reload
touch "$SHARED/tls-enabled"
echo 'HTTPS config active. Check https://alfacbcd.ru/; configure the renewal cron from README.'
