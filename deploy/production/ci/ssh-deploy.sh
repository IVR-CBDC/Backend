#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=ssh-common.sh
source "$HERE/ssh-common.sh"
setup_ssh
[[ "$RELEASE_ID" =~ ^b[0-9a-f]{12}-f[0-9a-f]{12}-r[0-9]+-a[0-9]+$ ]]
sha256sum -c release.tar.gz.sha256
root=/home/deploy/ivr-cbdc
ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" "umask 077; mkdir -p '$root/incoming/$RELEASE_ID' '$root/releases'"
scp "${SSH_OPTS[@]}" -P "$PROD_PORT" release.tar.gz release.tar.gz.sha256 "deploy@$PROD_HOST:$root/incoming/$RELEASE_ID/"
ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" "bash -s -- '$RELEASE_ID'" <<'REMOTE'
set -euo pipefail
umask 077
root=/home/deploy/ivr-cbdc
release_id="$1"
# Recheck initial setup immediately before mutation, including if it was revoked meanwhile.
test -f "$root/shared/auto-deploy-ready"
cd "$root/incoming/$release_id"
sha256sum -c release.tar.gz.sha256
test ! -e "$root/releases/$release_id"
mkdir "$root/releases/$release_id"
tar --no-same-owner -xzf release.tar.gz -C "$root/releases/$release_id"
rm release.tar.gz release.tar.gz.sha256
rmdir "$root/incoming/$release_id"
bash "$root/releases/$release_id/scripts/deploy.sh" --apply "$root/releases/$release_id"
REMOTE
