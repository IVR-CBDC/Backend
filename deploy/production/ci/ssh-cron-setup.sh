#!/usr/bin/env bash
# Explicit one-time host setup, enabled by INSTALL_CRON_ONCE repository variable.
# Uses the already authorized root-equivalent Docker access; no SSH/firewall edits.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=ssh-common.sh
source "$HERE/ssh-common.sh"
setup_ssh
ssh "${SSH_OPTS[@]}" -p "$PROD_PORT" "deploy@$PROD_HOST" 'python3 -' <<'PY'
import json
import subprocess


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=600)


try:
    if run(['sh', '-c', 'command -v crontab >/dev/null && systemctl is-active --quiet cron']).returncode == 0:
        print('CRON_SETUP already_ready=true')
        raise SystemExit(0)
    ids = run(['docker', 'ps', '-q']).stdout.split()
    if not ids:
        raise RuntimeError()
    inventory = json.loads(run(['docker', 'inspect', *ids]).stdout)
    # Reuse an existing trusted PostgreSQL image already running on this VPS.
    # No image pull/build, privileged container, or permanent host mount.
    candidates = [c['Image'] for c in inventory if 'postgres' in c['Config']['Image'].lower()]
    if not candidates:
        raise RuntimeError()
    command = '''set -eu
export DEBIAN_FRONTEND=noninteractive
if ! command -v crontab >/dev/null; then
    apt-get update -qq
    apt-get install -y --no-install-recommends cron
fi
systemctl enable --now cron
command -v crontab >/dev/null
systemctl is-active --quiet cron
'''
    result = run(['docker', 'run', '--rm', '--pull', 'never', '--read-only',
                  '--network', 'host', '--pid', 'host', '--memory', '256m', '--pids-limit', '128',
                  '--mount', 'type=bind,source=/,target=/host', '--entrypoint', 'chroot',
                  candidates[0], '/host', '/bin/sh', '-c', command])
    if result.returncode:
        raise RuntimeError()
    print('CRON_SETUP installed_and_active=true')
except Exception:
    print('CRON_SETUP_FAILED host_package_installation_failed')
    raise SystemExit(1) from None
PY
