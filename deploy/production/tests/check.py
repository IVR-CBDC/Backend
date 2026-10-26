#!/usr/bin/env python3
"""Static configuration + deployment safeguards. Does not start any containers."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

PROD = Path(__file__).resolve().parents[1]
REPO = PROD.parents[1]

def run(args, **kwargs):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)

def ok(args, **kwargs):
    r=run(args,**kwargs)
    if r.returncode: raise AssertionError(f'{args}: {r.stderr}\n{r.stdout}')
    return r.stdout

for path in list((PROD/'scripts').glob('*.sh'))+list((PROD/'ci').glob('*.sh')): ok(['bash','-n',str(path)])
for path in (PROD/'scripts').glob('*.py'): compile(path.read_text(),str(path),'exec')

with tempfile.TemporaryDirectory(prefix='ivr-prod-check-') as td:
    temp=Path(td)
    # Test the real package builder with a minimal fake checkout and a registry stub.
    fake=temp/'checkout'
    shutil.copytree(PROD,fake/'deploy/production',ignore=shutil.ignore_patterns('__pycache__'))
    for name in ['auth','core','commission']:
        shutil.copytree(REPO/f'services/service-{name}/migrations',fake/f'services/service-{name}/migrations')
    (fake/'infra/keys').mkdir(parents=True)
    shutil.copy2(REPO/'infra/migrate.sh',fake/'infra/migrate.sh')
    for path in ['.env','docker-compose.yml','infra/keys/jwt_private.jwk','deploy/production/.env','deploy/production/shared/secrets/db_auth','deploy/production/scripts/local-debug.sh','deploy/production/proxy/local.conf','services/service-auth/migrations/.env']:
        f=fake/path; f.parent.mkdir(parents=True,exist_ok=True); f.write_text('LOCAL_CANARY_MUST_NOT_SHIP')
    bin_dir=temp/'bin'; bin_dir.mkdir()
    docker=bin_dir/'docker'
    docker.write_text('#!/bin/sh\nprintf "sha256:'+ '1'*64 +'\\n"\n'); docker.chmod(0o755)
    env=dict(os.environ,PATH=str(bin_dir)+':'+os.environ['PATH'],BACKEND_SHA='a'*40,FRONTEND_SHA='b'*40)
    plan=temp/'image-plan.json'
    plan.write_text(json.dumps({'images':{k+'_IMAGE':'ghcr.io/ivr-cbdc/backend/service-'+v+':src-'+('a'*64) for k,v in [('AUTH','auth'),('CORE','core'),('COMMISSION','commission')]}}))
    env['IMAGE_PLAN']=str(plan)
    root=temp/'server'; release=root/'releases/test'; release.parent.mkdir(parents=True)
    ok([sys.executable,str(fake/'deploy/production/scripts/package.py'),str(release)],env=env)
    expected={'compose.yaml','server.env.example','README.md','scripts','proxy','images.env','release.json'}
    assert {p.name for p in release.iterdir()}==expected
    for f in release.rglob('*'):
        if f.is_file(): assert b'LOCAL_CANARY_MUST_NOT_SHIP' not in f.read_bytes(), f
    assert not list(release.rglob('*.sql'))
    assert (release/'scripts/secrets-entrypoint.sh').is_file()
    shared=root/'shared'; (shared/'secrets').mkdir(parents=True)
    for name in ['db_auth','db_core','db_commission','jwt_private.jwk','jwt_public.jwk','jwt_public.pem']:
        (shared/'secrets'/name).write_text('TEST_ONLY')
    server_env=shared/'server.env'
    server_env.write_text(f'SHARED_DIR={shared}\nDEPLOY_UID=1000\nDEPLOY_GID=1000\n')
    compose=['docker','compose','--env-file',str(server_env),'--env-file',str(release/'images.env'),'-f',str(release/'compose.yaml'),'--profile','tools','config','--format','json']
    cfg=json.loads(ok(compose))
    assert cfg['name']=='ivrcbdc-prod'
    total=0
    for name,s in cfg['services'].items():
        assert 'build' not in s, name
        assert '@sha256:' in s['image'], name
        assert not s.get('ports') or name=='proxy', name
        assert s['logging']['options']=={'max-size':'5m','max-file':'2'}, name
        assert not any('docker.sock' in str(v) for v in s.get('volumes',[])), name
        if not s.get('profiles'):
            total+=int(s['mem_limit'])
            assert 'healthcheck' in s, name
    assert total==864*1024*1024, total
    assert set(cfg['services'])=={'service-auth','service-core','service-commission','bff','frontend','proxy','certbot'}
    assert not cfg.get('volumes')
    assert cfg['networks']['private']['external'] is True
    assert cfg['services']['bff']['environment']['COOKIE_SECURE']=='true'
    assert cfg['services']['bff']['environment']['ALLOWED_ORIGINS']=='https://alfacbcd.ru'
    assert cfg['services']['certbot']['profiles']==['tools']
    assert not any('DB_PASSWORD' in s.get('environment',{}) for s in cfg['services'].values())
    assert 'commission-calculator' not in cfg['services']
    original=(release/'images.env').read_text()
    (release/'images.env').write_text(original.replace('@sha256:'+'1'*64,':main',1))
    assert run([sys.executable,str(release/'scripts/validate-release.py'),str(release)]).returncode
    (release/'images.env').write_text(original)
    # No pulls, stops or writes to a daemon if the disk budget is insufficient.
    calls=temp/'calls'
    docker.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$TEST_CALLS"\nif [ "$1" = info ]; then printf "%s\\n" "$TEST_DOCKER_ROOT"; fi\n'); docker.chmod(0o755)
    for name,script in {'id':'#!/bin/sh\necho deploy\n','df':'#!/bin/sh\nprintf "Avail\\n0\\n"\n'}.items():
        f=bin_dir/name; f.write_text(script); f.chmod(0o755)
    env.update(PROD_ROOT=str(root),TEST_CALLS=str(calls),TEST_DOCKER_ROOT=str(root),AUTH_IMAGE='LOCAL_MUST_NOT_OVERRIDE',COMPOSE_PROFILES='tools')
    r=run(['bash',str(release/'scripts/deploy.sh'),'--apply',str(release)],env=env)
    assert r.returncode and 'Insufficient free disk' in r.stderr, r.stderr
    assert all('config --quiet' in line or line.startswith('info ') for line in calls.read_text().splitlines())
    assert not (root/'current').exists()
    # All remaining deployment scenarios use Docker/curl stubs, never the daemon.
    (bin_dir/'df').write_text('#!/bin/sh\nprintf "Avail\n21474836480\n"\n')
    (bin_dir/'curl').write_text('#!/bin/sh\nprintf 503\n'); (bin_dir/'curl').chmod(0o755)
    (shared/'proxy').mkdir()
    before={f.name:f.read_bytes() for f in (shared/'secrets').iterdir()}
    docker.write_text("""#!/bin/sh
printf '%s\\n' "$*" >> "$TEST_CALLS"
if [ "$1" = info ]; then printf '%s\\n' "$TEST_DOCKER_ROOT"; fi
case "$*" in
  *" pull "*) [ "${FAIL_PULL:-0}" != 1 ] || exit 1;;
  *second/compose.yaml*" up -d --no-build --pull never --wait "*) [ "${FAIL_HEALTH:-0}" != 1 ] || exit 1;;
  *" --force-recreate "*) [ "${FAIL_ROLLBACK:-0}" != 1 ] || exit 1;;
esac
exit 0
""")
    def audit():
        log=calls.read_text()
        for forbidden in ['pg_dump','pg_restore','psql','migrate-','pg-auth','pg-core','pg-commission','redis',' down','volume rm','system prune','volume prune','--remove-orphans']:
            assert forbidden not in log, (forbidden,log)
        assert {f.name:f.read_bytes() for f in (shared/'secrets').iterdir()}==before
        assert not (shared/'backups').exists()
        assert 'LOCAL_MUST_NOT_OVERRIDE' not in log
        return log
    calls.write_text('')
    ok(['bash',str(release/'scripts/deploy.sh'),'--apply',str(release)],env=env)
    assert (root/'current').resolve()==release
    log=audit()
    assert log.index(' pull ')<log.index(' stop ')<log.index(' up ')
    second=root/'releases/second'; shutil.copytree(release,second)
    # Pull failure must leave the running release alone.
    calls.write_text('')
    r=run(['bash',str(second/'scripts/deploy.sh'),'--apply',str(second)],env=dict(env,FAIL_PULL='1'))
    assert r.returncode and ' stop ' not in audit()
    assert (root/'current').resolve()==release
    # Healthcheck failure must restore old application images, never databases.
    calls.write_text('')
    r=run(['bash',str(second/'scripts/deploy.sh'),'--apply',str(second)],env=dict(env,FAIL_HEALTH='1'))
    assert r.returncode and 'ROLLBACK_OK' in r.stderr, r.stderr
    assert (root/'current').resolve()==release and not (root/'previous').exists()
    assert 'up -d --no-deps --force-recreate --no-build --pull never --wait' in audit()
    calls.write_text('')
    r=run(['bash',str(second/'scripts/deploy.sh'),'--apply',str(second)],env=dict(env,FAIL_HEALTH='1',FAIL_ROLLBACK='1'))
    assert r.returncode and 'ROLLBACK_FAILED' in r.stderr and 'ROLLBACK_OK' not in r.stderr
    assert (root/'current').resolve()==release
    audit()
    # First failure has no prior images: stop only the application.
    (root/'current').unlink()
    calls.write_text('')
    r=run(['bash',str(second/'scripts/deploy.sh'),'--apply',str(second)],env=dict(env,FAIL_HEALTH='1'))
    assert r.returncode and 'No previous release' in r.stderr
    assert not (root/'current').exists()
    assert '--force-recreate' not in audit()
    # A subsequent success retains the former release for a future rollback.
    (root/'current').symlink_to(release)
    calls.write_text('')
    ok(['bash',str(second/'scripts/deploy.sh'),'--apply',str(second)],env=env)
    assert (root/'current').resolve()==second and (root/'previous').resolve()==release
    audit()

print('PASS: syntax; immutable images; application-only Compose; release allowlist; limits; low-disk/pull abort; promotion; health rollback and rollback-failure reporting; no database operations; secrets preserved')
