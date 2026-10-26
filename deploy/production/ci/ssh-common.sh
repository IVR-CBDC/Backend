#!/usr/bin/env bash
set -euo pipefail
setup_ssh() {
    local key missing=0
    : "${PROD_HOST:=}" "${PROD_SSH_KEY:=}" "${PROD_SSH_KNOWN_HOSTS:=}"
    for key in PROD_HOST PROD_SSH_KEY PROD_SSH_KNOWN_HOSTS; do
        if [[ -z "${!key:-}" ]]; then
            echo "Missing GitHub Secret: $key"
            missing=1
        fi
    done
    [[ "$missing" == 0 ]] || return 1
    PROD_PORT="${PROD_PORT:-22}"
    [[ "$PROD_HOST" =~ ^[a-zA-Z0-9][a-zA-Z0-9.-]*$ ]] || return 1
    [[ "$PROD_PORT" =~ ^[0-9]+$ && "$PROD_PORT" -ge 1 && "$PROD_PORT" -le 65535 ]] || return 1
    umask 077
    SSH_DIR=$(mktemp -d)
    trap 'rm -rf -- "$SSH_DIR"' EXIT
    printf '%s\n' "$PROD_SSH_KEY" > "$SSH_DIR/key"
    printf '%s\n' "$PROD_SSH_KNOWN_HOSTS" > "$SSH_DIR/known_hosts"
    ssh-keygen -y -P '' -f "$SSH_DIR/key" > /dev/null || return 1
    # Used by the scripts sourcing this helper.
    # shellcheck disable=SC2034
    SSH_OPTS=(-F /dev/null -i "$SSH_DIR/key" -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$SSH_DIR/known_hosts" -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=4)
}
