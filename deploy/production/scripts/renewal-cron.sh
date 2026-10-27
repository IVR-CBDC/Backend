#!/usr/bin/env bash
# Installs only our tagged job; preserves all other crontab entries.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=common.sh
source "$HERE/common.sh"
lock_deploy
[[ -f "$SHARED/tls-enabled" ]] || fail 'TLS must be enabled before scheduling renewal'
command -v crontab >/dev/null || fail 'crontab is unavailable'
systemctl is-active --quiet cron || fail 'cron service is not running'
current=$(mktemp)
updated=$(mktemp)
trap 'rm -f -- "$current" "$updated"' EXIT
crontab -l > "$current" 2>/dev/null || : > "$current"
awk 'index($0, "# ivrcbdc-managed-renewal") == 0' "$current" > "$updated"
printf '17 3,15 * * * PATH=/usr/local/bin:/usr/bin:/bin /bin/bash %s/current/scripts/tls.sh --renew 2>&1 | /usr/bin/logger -t ivrcbdc-tls # ivrcbdc-managed-renewal\n' "$PROD_ROOT" >> "$updated"
crontab "$updated"
crontab -l | grep -Fq -- '# ivrcbdc-managed-renewal' || fail 'Renewal cron installation could not be verified'
echo 'Automatic certificate renewal scheduled twice daily.'
