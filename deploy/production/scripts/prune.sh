#!/usr/bin/env bash
# Invoked by deploy while holding its lock. Never use docker system/volume prune.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=common.sh
source "$HERE/common.sh"
if [[ "${1:-}" != --already-locked ]]; then lock_deploy; fi
[[ -L "$PROD_ROOT/current" ]] || fail 'No successful release to retain'
python3 - "$PROD_ROOT" <<'PYCODE'
import json, pathlib, subprocess, sys
root=pathlib.Path(sys.argv[1])
keep={p.resolve() for p in [root/'current', root/'previous'] if p.exists()}
retained={v for p in keep for v in json.loads((p/'release.json').read_text())['images'].values()}
# Only references recorded by this deployment, and only this project's application repositories.
# Keep old metadata for audit/retry; each release is small, and metadata is not a disk pressure source.
known={v for p in (root/'releases').glob('*/release.json') for v in json.loads(p.read_text())['images'].values()}
for ref in sorted(known-retained):
    if ref.startswith(('ghcr.io/ivr-cbdc/backend/','ghcr.io/ivr-cbdc/frontend/')):
        # No --force: Docker protects images still referenced by ANY container.
        subprocess.run(['docker','image','rm',ref],check=False)
PYCODE
