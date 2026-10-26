#!/usr/bin/env bash
set -euo pipefail
umask 077
PROD_ROOT="${PROD_ROOT:-/home/deploy/ivr-cbdc}"
[[ "$PROD_ROOT" =~ ^/[a-zA-Z0-9_/-]+$ ]] || { echo 'Invalid PROD_ROOT' >&2; exit 1; }
SHARED="$PROD_ROOT/shared"
fail() { echo "ERROR: $*" >&2; return 1; }
lock_deploy() {
    mkdir -p "$PROD_ROOT"
    exec 9>"$PROD_ROOT/.deploy.lock"
    flock -w 1800 9 || fail 'Timed out waiting for the deployment/TLS lock'
}
load_release() {
    RELEASE=$(realpath "${1:-$PROD_ROOT/current}")
    [[ "$RELEASE" == "$PROD_ROOT/releases/"* ]] || fail 'Release must be under PROD_ROOT/releases'
    python3 "$RELEASE/scripts/validate-release.py" "$RELEASE"
    [[ -f "$SHARED/server.env" ]] || fail 'Configure existing production connections/secrets first'
    # Shell variables must never override the reviewed release or enable tool profiles.
    unset AUTH_IMAGE CORE_IMAGE COMMISSION_IMAGE BFF_IMAGE FRONTEND_IMAGE CERTBOT_IMAGE
    unset SHARED_DIR DEPLOY_UID DEPLOY_GID COMPOSE_PROFILES COMPOSE_ENV_FILES
    export COMPOSE_DISABLE_ENV_FILE=true
    for secret in db_auth db_core db_commission jwt_private.jwk jwt_public.jwk jwt_public.pem; do
        [[ -s "$SHARED/secrets/$secret" ]] || fail "Missing secret file: $secret; connect existing production secrets"
    done
    # Used by scripts sourcing this file.
    # shellcheck disable=SC2034
    DC=(docker compose --project-name ivrcbdc-prod --env-file "$SHARED/server.env" --env-file "$RELEASE/images.env" -f "$RELEASE/compose.yaml")
}
space_guard() {
    local need="$1" path free docker_root
    docker_root=$(docker info --format '{{.DockerRootDir}}')
    for path in "$PROD_ROOT" "$docker_root" /var/lib/containerd; do
        [[ -e "$path" ]] || continue
        free=$(df -B1 --output=avail "$path" | tail -n 1 | tr -d ' ')
        (( free >= need )) || fail "Insufficient free disk at $path: $free bytes; need $need. No automatic global prune."
    done
}
