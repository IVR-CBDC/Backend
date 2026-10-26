#!/usr/bin/env python3
import json
import pathlib
import re
import sys
root = pathlib.Path(sys.argv[1])
expected = {
    'AUTH_IMAGE': 'ghcr.io/ivr-cbdc/backend/service-auth',
    'CORE_IMAGE': 'ghcr.io/ivr-cbdc/backend/service-core',
    'COMMISSION_IMAGE': 'ghcr.io/ivr-cbdc/backend/service-commission',
    'BFF_IMAGE': 'ghcr.io/ivr-cbdc/frontend/bff',
    'FRONTEND_IMAGE': 'ghcr.io/ivr-cbdc/frontend/spa',
    'CERTBOT_IMAGE': 'certbot/certbot',
}
values = {}
for line in (root/'images.env').read_text().splitlines():
    if not line or line.startswith('#'): continue
    key, value = line.split('=', 1)
    if key not in expected or key in values: raise SystemExit('Unexpected/duplicate image variable')
    if not re.fullmatch(re.escape(expected[key]) + r'@sha256:[0-9a-f]{64}', value):
        raise SystemExit(f'Invalid immutable image reference: {key}')
    values[key] = value
if values.keys() != expected.keys(): raise SystemExit('Missing image variables')
meta = json.loads((root/'release.json').read_text())
if meta['images'] != values: raise SystemExit('Manifest differs from images.env')
for key in ['backend_sha','frontend_sha']:
    if not re.fullmatch('[0-9a-f]{40}',meta[key]): raise SystemExit('Invalid source SHA')
print('Release manifest: valid, all images pinned by digest')
