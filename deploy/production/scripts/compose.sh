#!/usr/bin/env bash
# Operator helper; uses exactly the current release and persistent project name.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=common.sh
source "$HERE/common.sh"
load_release
exec "${DC[@]}" "$@"
