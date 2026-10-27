#!/usr/bin/env bash
# Explicitly enabled for the first installation; never part of routine app deploys.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=ssh-common.sh
source "$HERE/ssh-common.sh"
setup_ssh
python3 "$HERE/package-bootstrap.py" "$SSH_DIR/bootstrap.py"
ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" 'python3 - --apply' < "$SSH_DIR/bootstrap.py"
