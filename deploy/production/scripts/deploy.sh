#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=common.sh
source "$HERE/common.sh"
[[ "${1:-}" == --apply && $# == 2 ]] || fail 'Usage: deploy.sh --apply RELEASE_DIRECTORY'
[[ $(id -un) == deploy ]] || fail 'Run as deploy'
[[ $(uname -m) == x86_64 ]] || fail 'This release targets linux/amd64'
lock_deploy
load_release "$2"
TARGET="$RELEASE"
OLD=''
if [[ -L "$PROD_ROOT/current" ]]; then OLD=$(readlink -f "$PROD_ROOT/current"); fi
APPS=(proxy bff service-core service-auth service-commission frontend)
mode=http
[[ ! -f "$SHARED/tls-enabled" ]] || mode=https

install_proxy() {
    cp "$RELEASE/proxy/$mode.conf" "$SHARED/proxy/default.conf.new"
    chmod 644 "$SHARED/proxy/default.conf.new"
    mv "$SHARED/proxy/default.conf.new" "$SHARED/proxy/default.conf"
}
check_ingress() {
    if [[ "$mode" == https ]]; then
        curl --noproxy '*' --connect-timeout 5 --max-time 15 --fail --silent --show-error --resolve alfacbcd.ru:443:127.0.0.1 https://alfacbcd.ru/ -o /dev/null
    else
        [[ $(curl --noproxy '*' --connect-timeout 5 --max-time 15 --silent --output /dev/null --write-out '%{http_code}' -H 'Host: alfacbcd.ru' http://127.0.0.1/) == 503 ]]
    fi
}
rollback() {
    local status="$1" restore_status
    trap - ERR INT TERM HUP
    set +e
    echo 'Deployment failed; restoring previously running images without changing database contents.' >&2
    "${DC[@]}" stop "${APPS[@]}"
    if [[ -n "$OLD" ]]; then
        # Separate errexit scope: a failed restore must not be reported as successful.
        (
            set -e
            load_release "$OLD"
            install_proxy
            "${DC[@]}" up -d --no-deps --force-recreate --no-build --pull never --wait --wait-timeout 240 service-auth service-core service-commission
            "${DC[@]}" up -d --no-deps --force-recreate --no-build --pull never --wait --wait-timeout 240 bff frontend proxy
            check_ingress
        )
        restore_status=$?
        if [[ "$restore_status" == 0 ]]; then
            echo "ROLLBACK_OK: restored $OLD; databases were NOT deployed or modified by this script." >&2
            bash "$TARGET/scripts/prune.sh" --already-locked || echo 'Rollback succeeded; image cleanup needs attention' >&2
        else
            echo "ROLLBACK_FAILED: data preserved; inspect application health and existing database connectivity." >&2
        fi
    else
        echo 'No previous release on first deployment. Application stopped; existing databases and secrets untouched.' >&2
    fi
    exit "$status"
}

"${DC[@]}" config --quiet
space_guard $((3*1024*1024*1024))
COMPOSE_PARALLEL_LIMIT=1 "${DC[@]}" pull service-auth service-core service-commission bff frontend proxy
space_guard $((1024*1024*1024))
trap 'rollback "$?"' ERR
trap 'rollback 130' INT
trap 'rollback 143' TERM
trap 'rollback 129' HUP
"${DC[@]}" stop "${APPS[@]}"
install_proxy
"${DC[@]}" up -d --no-build --pull never --wait --wait-timeout 240
check_ingress
if [[ -n "$OLD" && "$OLD" != "$TARGET" ]]; then
    ln -sfn "$OLD" "$PROD_ROOT/previous.new"
    mv -Tf "$PROD_ROOT/previous.new" "$PROD_ROOT/previous"
fi
ln -sfn "$TARGET" "$PROD_ROOT/current.new"
mv -Tf "$PROD_ROOT/current.new" "$PROD_ROOT/current"
trap - ERR INT TERM HUP
bash "$RELEASE/scripts/prune.sh" --already-locked
echo "Deployment healthy: $RELEASE; ingress mode: $mode"
