#!/usr/bin/env bash
# Configure optional registry credentials, then audit without starting production or changing databases.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=ssh-common.sh
source "$HERE/ssh-common.sh"
echo 'ready=false' >> "$GITHUB_OUTPUT"
if ! setup_ssh; then
    echo 'Production not ready: configure SSH Secrets / valid unencrypted key and verified host key.' >> "$GITHUB_STEP_SUMMARY"
    exit 0
fi
if [[ -n "${GHCR_READ_TOKEN:-}" ]]; then
    if [[ ! "${GHCR_USERNAME:-}" =~ ^[A-Za-z0-9][A-Za-z0-9-]*$ ]]; then
        echo 'Missing valid GHCR_USERNAME repository variable' >> "$GITHUB_STEP_SUMMARY"
        exit 0
    fi
    # The read-only registry credential goes directly into Docker over SSH stdin.
    # Never print it, pass it as an argument, or upload it in an artifact.
    if ! printf '%s' "$GHCR_READ_TOKEN" | ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" \
        "docker login ghcr.io --username '$GHCR_USERNAME' --password-stdin" > /dev/null; then
        echo 'Registry authentication failed; production not started.' >> "$GITHUB_STEP_SUMMARY"
        exit 0
    fi
fi
ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" 'python3 -' < "$HERE/inspect-connections.py" || echo 'Bootstrap capability checks incomplete'
if ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" 'bash -s' <<'REMOTE'
set -euo pipefail
root=/home/deploy/ivr-cbdc
missing=0
check() {
    local label="$1"
    shift
    if ! "$@"; then echo "MISSING: $label"; missing=1; fi
}
check 'deploy user' test "$(id -un)" = deploy
check 'amd64 architecture' test "$(uname -m)" = x86_64
check 'Docker access' docker info --format '{{.ServerVersion}}'
check 'Compose' docker compose version --short
for tool in python3 curl flock timeout tar; do check "$tool" command -v "$tool"; done
check 'server initialization: shared/server.env' test -s "$root/shared/server.env"
for name in db_auth db_core db_commission jwt_private.jwk jwt_public.jwk jwt_public.pem; do
    check "production secret file $name" test -s "$root/shared/secrets/$name"
done
check 'existing services configured: shared/auto-deploy-ready' test -f "$root/shared/auto-deploy-ready"
network=$(python3 - "$root/shared/server.env" <<'PYCODE'
import pathlib,sys
p=pathlib.Path(sys.argv[1])
v=dict(line.split('=',1) for line in p.read_text().splitlines() if line and not line.startswith('#') and '=' in line) if p.exists() else {}
print(v.get('APP_NETWORK','ivrcbdc-prod_private'))
PYCODE
)
check 'existing application/data network' bash -c 'docker network inspect "$1" >/dev/null' _ "$network"
check 'registry login for private GHCR images' test -s "$HOME/.docker/config.json"
for path in /home/deploy "$(docker info --format '{{.DockerRootDir}}')" /var/lib/containerd; do
    [[ -e "$path" ]] || continue
    available=$(df -B1 --output=avail "$path" | tail -n1 | tr -d ' ')
    check 'at least 3 GiB free for a safe update' test "$available" -ge 3221225472
done
exit "$missing"
REMOTE
then
    echo 'ready=true' >> "$GITHUB_OUTPUT"
    echo 'Production readiness checks passed; no approval step is required.' >> "$GITHUB_STEP_SUMMARY"
else
    echo 'Production NOT started. Read-only SSH preflight failed; see MISSING lines or SSH error in this job.' >> "$GITHUB_STEP_SUMMARY"
fi
