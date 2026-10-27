#!/usr/bin/env python3
"""Integration test in exclusively owned disposable PostgreSQL/Redis containers.
The existing local stack is never selected as a bootstrap target.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

prod = Path(__file__).resolve().parents[1]
repo = prod.parents[1]
spec = importlib.util.spec_from_file_location('bootstrap', prod/'ci/bootstrap-host.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
schemas = {service: {p.name: subprocess.check_output(['git', '-C', str(repo), 'show', 'HEAD:'+str(p.relative_to(repo))], text=True)
                     for p in sorted((repo/f'services/service-{service}/migrations').glob('*.sql'))}
           for service in module.SERVICES}
prefix = 'ivr-bootstrap-check-'+uuid.uuid4().hex[:10]
network, pg, redis = prefix+'-net', prefix+'-pg', prefix+'-redis'
created = []
# Deliberately not hex: exercise spaces, quotes, backslashes, URL punctuation and
# a trailing file newline without ever using real production credentials.
test_password = " Synthetic'\\pass:$%/@?= only "


def docker(*args):
    return subprocess.check_output(['docker', *args], text=True, stderr=subprocess.PIPE).strip()


try:
    docker('network', 'create', network)
    docker('run', '-d', '--pull', 'never', '--name', pg, '--network', network, '--memory', '256m',
           '--tmpfs', '/var/lib/postgresql/data:rw,size=128m', '-e', 'POSTGRES_PASSWORD='+test_password, 'postgres:16-alpine')
    created.append(pg)
    docker('run', '-d', '--pull', 'never', '--name', redis, '--network', network, '--memory', '64m',
           '--tmpfs', '/data:rw,size=16m', 'redis:7-alpine', 'redis-server', '--save', '', '--appendonly', 'no')
    created.append(redis)
    for _ in range(60):
        ready = subprocess.run(['docker', 'exec', pg, 'pg_isready', '-h', '127.0.0.1', '-U', 'postgres'], capture_output=True).returncode == 0
        if ready:
            break
        time.sleep(0.5)
    assert ready, 'Disposable PostgreSQL did not become ready'
    inventory = json.loads(docker('inspect', pg, redis))
    assert {c['Name'].lstrip('/') for c in inventory} == {pg, redis}
    pg_info = next(c for c in inventory if c['Name'].lstrip('/') == pg)
    admin = module.db_client(pg_info)
    admin('CREATE DATABASE unrelated;')
    unrelated = module.db_client(pg_info, database='unrelated')
    unrelated("CREATE TABLE sentinel(value TEXT); INSERT INTO sentinel VALUES ('do not touch');")
    with tempfile.TemporaryDirectory(prefix='ivr-bootstrap-test-') as temporary:
        root = Path(temporary)/'server'
        directory = root/'shared/secrets'
        directory.mkdir(parents=True)
        for service in module.SERVICES:
            path = directory/('db_'+service)
            path.write_text(test_password+'\n')
            path.chmod(0o600)
        original_secrets = {p.name: p.read_bytes() for p in directory.iterdir()}
        # Even if the first two passwords are valid, the third invalid password
        # must stop before any application role/database/schema is created.
        (directory/'db_commission').write_text('INCORRECT_SYNTHETIC_PASSWORD\n')
        try:
            module.initialize(root, schemas, inventory=inventory)
        except module.BootstrapError as error:
            assert str(error) == 'existing_db_secret_connection_failed', str(error)
        else:
            raise AssertionError('Invalid existing credentials were accepted')
        assert admin("SELECT count(*) FROM pg_roles WHERE rolname LIKE 'ivrcbdc_%';") == '0'
        assert admin("SELECT count(*) FROM pg_database WHERE datname LIKE 'ivrcbdc_%';") == '0'
        assert (directory/'db_commission').read_text() == 'INCORRECT_SYNTHETIC_PASSWORD\n'
        (directory/'db_commission').write_bytes(original_secrets['db_commission'])
        module.initialize(root, schemas, inventory=inventory)
        assert all((directory/name).read_bytes() == content for name, content in original_secrets.items())
        def snapshot():
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (root/'shared/secrets').iterdir() if p.is_file()}
        before = snapshot()
        assert set(before) == set(module.SECRET_FILES)
        assert all(p.stat().st_mode & 0o777 == 0o600 for p in (root/'shared/secrets').iterdir())
        module.initialize(root, schemas, inventory=inventory)
        assert snapshot() == before, 'Repeated initialization rotated secrets'
        assert unrelated('SELECT value FROM sentinel;') == 'do not touch'
        for service in module.SERVICES:
            password = (root/'shared/secrets'/('db_'+service)).read_text().rstrip('\n')
            client = module.db_client(pg_info, user='ivrcbdc_'+service, password=password, database='ivrcbdc_'+service)
            assert client('SELECT count(*) FROM schema_migrations;') == str(len(schemas[service]))
            assert admin("SELECT rolsuper OR rolcreatedb OR rolcreaterole FROM pg_roles WHERE rolname='ivrcbdc_"+service+"';") == 'f'
            if service in ('auth', 'core'):
                table = 'users' if service == 'auth' else 'deals'
                assert client('SELECT count(*) FROM '+table+';') == '0'
            else:
                assert int(client('SELECT count(*) FROM corridors;')) > 0
        # A resumed initialization after loss of the completion marker remains idempotent.
        (root/'shared/bootstrap-complete.json').unlink()
        module.initialize(root, schemas, inventory=inventory)
        assert snapshot() == before
        # Never silently replace a partial JWT set.
        (root/'shared/secrets/jwt_public.pem').unlink()
        try:
            module.generate_keys(root/'shared/secrets')
        except module.BootstrapError:
            pass
        else:
            raise AssertionError('Partial existing key set should be refused')
    # Unrelated existing databases/roles with the same names cannot be adopted.
    with tempfile.TemporaryDirectory(prefix='ivr-bootstrap-conflict-') as temporary:
        root = Path(temporary)/'server'
        try:
            module.initialize(root, schemas, inventory=inventory)
        except module.BootstrapError as error:
            assert str(error) == 'existing_db_secret_missing_refusing_generation'
        else:
            raise AssertionError('Missing database passwords must not be generated')
        assert not list((root/'shared/secrets').iterdir())
        for service in module.SERVICES:
            (root/'shared/secrets'/('db_'+service)).write_text(test_password+'\n')
        try:
            module.initialize(root, schemas, inventory=inventory)
        except module.BootstrapError as error:
            assert str(error) == 'existing_app_database_or_role_refusing_adoption'
        else:
            raise AssertionError('Unowned existing databases must not be adopted')
    print('PASS: existing non-hex secrets authenticated and preserved byte-for-byte; invalid password stops before SQL writes; real PostgreSQL schema/reference initialization; restricted app roles; empty users/deals; existing unrelated data preserved; retry/resume keep secrets; partial keys/conflicting databases refused')
finally:
    for name in reversed(created):
        subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    subprocess.run(['docker', 'network', 'rm', network], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
