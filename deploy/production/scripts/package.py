#!/usr/bin/env python3
"""Run on GitHub runner after images have been published, never on VPS."""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
repo = pathlib.Path(__file__).resolve().parents[3]
prod = repo/'deploy/production'
out = pathlib.Path(sys.argv[1]).resolve()
if out.exists(): raise SystemExit('Output directory already exists')
backend, frontend = os.environ['BACKEND_SHA'], os.environ['FRONTEND_SHA']
for sha in (backend,frontend):
    if not re.fullmatch('[0-9a-f]{40}',sha): raise SystemExit('Expected full Git SHA')
plan=json.loads(pathlib.Path(os.environ['IMAGE_PLAN']).read_text())
refs=dict(plan['images'])
if set(refs)!={'AUTH_IMAGE','CORE_IMAGE','COMMISSION_IMAGE'}: raise SystemExit('Invalid backend image plan')
refs.update(BFF_IMAGE='ghcr.io/ivr-cbdc/frontend/bff:sha-'+frontend, FRONTEND_IMAGE='ghcr.io/ivr-cbdc/frontend/spa:sha-'+frontend)
for line in (prod/'infra-images.env').read_text().splitlines():
    if not line or line.startswith('#'): continue
    key,value=line.split('=',1)
    refs[key.replace('_SOURCE','_IMAGE')]=value
images={}
for key, ref in refs.items():
    # buildx registry metadata only; runtime layers are NOT downloaded to the runner.
    attempts=30 if key in {'BFF_IMAGE','FRONTEND_IMAGE'} else 1
    for attempt in range(attempts):
        result=subprocess.run(['docker','buildx','imagetools','inspect',ref,'--format','{{.Manifest.Digest}}'],text=True,capture_output=True)
        if result.returncode==0: break
        if attempt+1==attempts: raise SystemExit('Image unavailable; check publication/package access: '+ref)
        time.sleep(10)
    digest=result.stdout.strip()
    if not re.fullmatch('sha256:[0-9a-f]{64}',digest): raise SystemExit('Invalid manifest digest')
    repository=ref.split('@')[0].rsplit(':',1)[0]
    images[key]=repository+'@'+digest
out.mkdir(parents=True)
for name in ['compose.yaml','server.env.example','README.md']:
    shutil.copy2(prod/name,out/name)
(out/'scripts').mkdir()
for name in ['common.sh','validate-release.py','deploy.sh','prune.sh','tls.sh','compose.sh','secrets-entrypoint.sh']:
    shutil.copy2(prod/'scripts'/name,out/'scripts'/name)
(out/'proxy').mkdir()
for name in ['http.conf','https.conf']:
    shutil.copy2(prod/'proxy'/name,out/'proxy'/name)
(out/'images.env').write_text(''.join(k+'='+v+'\n' for k,v in images.items()))
(out/'release.json').write_text(json.dumps(dict(backend_sha=backend,frontend_sha=frontend,images=images,run_url=os.environ.get('RUN_URL','')),indent=2)+'\n')
subprocess.run([sys.executable,str(out/'scripts/validate-release.py'),str(out)],check=True)
print('Release bundle ready:',out)
