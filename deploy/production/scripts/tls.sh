#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=common.sh
source "$HERE/common.sh"
mode="${1:-}"
registration=()
case "$mode" in
    --issue)
        [[ $# == 3 && "$3" == --accept-terms && "$2" == *@* ]] || fail 'Explicit email and --accept-terms required'
        registration=(--email "$2")
        ;;
    --issue-without-email)
        [[ $# == 2 && "$2" == --accept-terms ]] || fail 'Explicit --accept-terms required'
        registration=(--register-unsafely-without-email)
        ;;
    --renew) [[ $# == 1 ]] || fail 'Usage: tls.sh --renew' ;;
    *) fail 'Usage: tls.sh --issue EMAIL --accept-terms | --issue-without-email --accept-terms | --renew' ;;
esac
lock_deploy
load_release
[[ "$mode" != --renew || -f "$SHARED/tls-enabled" ]] || fail 'TLS has not been enabled'
space_guard $((1024*1024*1024))
# nginx workers must be able to traverse the public ACME webroot.
mkdir -p "$SHARED/acme"
chmod 755 "$SHARED/acme"
"${DC[@]}" --profile tools pull certbot
if [[ "$mode" != --renew ]]; then
    "${DC[@]}" --profile tools run --rm --no-deps -T certbot certonly --non-interactive \
        --webroot -w /var/www/acme --domain alfacbcd.ru --cert-name alfacbcd.ru \
        "${registration[@]}" --agree-tos --no-eff-email
fi
previous=$(mktemp "$SHARED/proxy/.before-tls.XXXXXX")
trap 'rm -f -- "$previous"' EXIT
cp "$SHARED/proxy/default.conf" "$previous"
restore_proxy() {
    cp "$previous" "$SHARED/proxy/default.conf.new"
    chmod 644 "$SHARED/proxy/default.conf.new"
    mv "$SHARED/proxy/default.conf.new" "$SHARED/proxy/default.conf"
    "${DC[@]}" exec -T proxy nginx -t && "${DC[@]}" exec -T proxy nginx -s reload
}
if [[ "$mode" != --renew ]]; then
    cp "$RELEASE/proxy/https.conf" "$SHARED/proxy/default.conf.new"
    chmod 644 "$SHARED/proxy/default.conf.new"
    mv "$SHARED/proxy/default.conf.new" "$SHARED/proxy/default.conf"
else
    "${DC[@]}" --profile tools run --rm --no-deps -T certbot renew --non-interactive --quiet
fi
if ! "${DC[@]}" exec -T proxy nginx -t || ! "${DC[@]}" exec -T proxy nginx -s reload; then
    restore_proxy || true
    fail 'nginx rejected TLS; previous proxy config and all certificate files preserved'
fi
if ! curl --noproxy '*' --retry 5 --retry-connrefused --retry-delay 1 --connect-timeout 5 --max-time 15 \
    --fail --silent --show-error --resolve alfacbcd.ru:443:127.0.0.1 https://alfacbcd.ru/ -o /dev/null; then
    restore_proxy || true
    fail 'HTTPS verification failed; previous proxy config restored'
fi
touch "$SHARED/tls-enabled"
echo 'HTTPS certificate/configuration verified successfully.'
