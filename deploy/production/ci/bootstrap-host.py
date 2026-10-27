#!/usr/bin/env python3
"""One-time authorized initialization inside EXISTING PostgreSQL/Redis services.
Only app-owned databases/roles/files are created. No server credentials or topology
are printed; SQL comes from a committed, explicit allowlist injected by the runner.
"""
import base64
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import uuid

SERVICES = ('auth', 'core', 'commission')
SECRET_FILES = ('db_auth', 'db_core', 'db_commission', 'jwt_private.jwk', 'jwt_public.jwk', 'jwt_public.pem')


class BootstrapError(Exception):
    pass


def execute(args, *, value=None, env=None, stage='command_failed'):
    try:
        r = subprocess.run(args, input=value, text=True, capture_output=True, env=env, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        raise BootstrapError(stage) from None
    if r.returncode:
        raise BootstrapError(stage)
    return r.stdout


def literal(value):
    return "'" + value.replace("'", "''") + "'"


def atomic_new(path, content):
    """Create without ever overwriting an existing file or following its symlink."""
    if path.exists() or path.is_symlink():
        if path.read_text() != content:
            raise BootstrapError('existing_file_differs_refusing_overwrite')
        return
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as f:
        temporary = Path(f.name)
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    try:
        os.link(temporary, path)
    finally:
        temporary.unlink()


def environment(container):
    return dict(v.split('=', 1) for v in container['Config'].get('Env', []) if '=' in v)


def running_containers():
    ids = execute(['docker', 'ps', '-q']).split()
    return json.loads(execute(['docker', 'inspect', *ids])) if ids else []


def choose_targets(inventory):
    postgres = [c for c in inventory if 'postgres' in c['Config']['Image'].lower()]
    redis = [c for c in inventory if 'redis' in c['Config']['Image'].lower()]
    if not postgres or len(redis) != 1:
        raise BootstrapError('existing_data_services_ambiguous_or_missing')
    targets = {}
    for service in SERVICES:
        matches = [c for c in postgres if environment(c).get('POSTGRES_DB') == service
                   or re.search(r'(^|[-_/])' + service + r'($|[-_/])', c['Name'].lstrip('/'))]
        if len(matches) == 1:
            targets[service] = matches[0]
        elif len(postgres) == 1:
            targets[service] = postgres[0]
        else:
            raise BootstrapError('existing_postgres_target_ambiguous')
    networks = [set(c['NetworkSettings']['Networks']) - {'bridge', 'host', 'none'}
                for c in list(targets.values()) + redis]
    common = set.intersection(*networks)
    if len(common) != 1:
        raise BootstrapError('existing_shared_network_ambiguous_or_missing')
    pong = execute(['docker', 'exec', redis[0]['Id'], 'redis-cli', '--raw', 'PING'])
    if pong.strip() != 'PONG':
        raise BootstrapError('redis_requires_unsupported_authentication')
    return targets, redis[0], common.pop()


def db_client(container, user=None, password=None, database='postgres'):
    config = environment(container)
    if password is None:
        password = config.get('POSTGRES_PASSWORD', '')
        if not password and config.get('POSTGRES_PASSWORD_FILE'):
            password = execute(['docker', 'exec', container['Id'], 'cat', config['POSTGRES_PASSWORD_FILE']]).rstrip('\n')
    client_env = dict(os.environ, PGPASSWORD=password,
                      PGOPTIONS='-c statement_timeout=60000 -c lock_timeout=15000 -c standard_conforming_strings=on', PGCONNECT_TIMEOUT='10')
    # Use the container network address: localhost may use trust authentication
    # in PostgreSQL images, which would accept even an incorrect password.
    cmd = ['docker', 'exec', '-i', '-e', 'PGPASSWORD', '-e', 'PGOPTIONS', '-e', 'PGCONNECT_TIMEOUT',
           container['Id'], 'psql', '-X', '-w', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1',
           '-h', container['Name'].lstrip('/'), '-U', user or config.get('POSTGRES_USER', 'postgres'), '-d', database]
    def query(sql):
        return execute(cmd, value=sql, env=client_env, stage='database_query_failed').strip()
    return query


def integer_der(value):
    raw = value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big')
    if raw[0] & 128:
        raw = b'\0' + raw
    return b'\x02' + der_length(len(raw)) + raw


def der_length(length):
    if length < 128:
        return bytes([length])
    raw = length.to_bytes((length.bit_length() + 7) // 8, 'big')
    return bytes([128 + len(raw)]) + raw


def rsa_numbers(raw):
    def field(position):
        tag, length = raw[position], raw[position + 1]
        start = position + 2
        if length & 128:
            count = length & 127
            length = int.from_bytes(raw[start:start + count], 'big')
            start += count
        return tag, start, start + length
    tag, cursor, end = field(0)
    if tag != 48 or end != len(raw):
        raise BootstrapError('invalid_generated_key')
    numbers = []
    while cursor < end:
        tag, start, cursor = field(cursor)
        if tag != 2:
            raise BootstrapError('invalid_generated_key')
        numbers.append(int.from_bytes(raw[start:cursor], 'big'))
    if len(numbers) != 9 or numbers[0] != 0:
        raise BootstrapError('invalid_generated_key')
    return dict(zip(('n', 'e', 'd', 'p', 'q', 'dp', 'dq', 'qi'), numbers[1:]))


def generate_keys(directory):
    paths = [directory/n for n in SECRET_FILES[3:]]
    if any(p.exists() for p in paths):
        if not all(p.is_file() and p.stat().st_size for p in paths):
            raise BootstrapError('partial_existing_jwt_keys_refusing_rotation')
        private, public = [json.loads(p.read_text()) for p in paths[:2]]
        if private.get('kty') != 'RSA' or any(private.get(k) != public.get(k) for k in ('n', 'e', 'kid')):
            raise BootstrapError('existing_jwt_key_mismatch')
        values = {k: int.from_bytes(base64.urlsafe_b64decode(private[k]+'='*(-len(private[k]) % 4)), 'big')
                  for k in ('n', 'e', 'd', 'p', 'q', 'dp', 'dq', 'qi')}
        if (values['n'] != values['p']*values['q'] or
            values['d']*values['e'] % math.lcm(values['p']-1, values['q']-1) != 1 or
            values['dp'] != values['d'] % (values['p']-1) or values['dq'] != values['d'] % (values['q']-1) or
            values['qi']*values['q'] % values['p'] != 1):
            raise BootstrapError('invalid_existing_private_key')
        encoded = integer_der(values['n'])+integer_der(values['e'])
        expected = b'\x30'+der_length(len(encoded))+encoded
        result = subprocess.run(['openssl', 'rsa', '-pubin', '-in', str(paths[2]), '-RSAPublicKey_out', '-outform', 'DER'], capture_output=True, timeout=15)
        if result.returncode or result.stdout != expected:
            raise BootstrapError('existing_public_pem_mismatch')
        return
    # Only a new installation gets new keys; no existing secret can be replaced.
    with tempfile.TemporaryDirectory(prefix='jwt-init-', dir=directory) as temporary:
        pem = Path(temporary)/'private.pem'
        der = Path(temporary)/'private.der'
        execute(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', str(pem)])
        execute(['openssl', 'rsa', '-in', str(pem), '-traditional', '-outform', 'DER', '-out', str(der)])
        numbers = rsa_numbers(der.read_bytes())
        encoded = {k: base64.urlsafe_b64encode(v.to_bytes((v.bit_length()+7)//8, 'big')).rstrip(b'=').decode()
                   for k, v in numbers.items()}
        private = dict(kty='RSA', use='sig', alg='RS256', kid='prod-'+uuid.uuid4().hex, **encoded)
        public = {k: private[k] for k in ('kty', 'use', 'alg', 'kid', 'n', 'e')}
        public_pem = execute(['openssl', 'pkey', '-in', str(pem), '-pubout'])
        for path, content in zip(paths, [json.dumps(private)+'\n', json.dumps(public)+'\n', public_pem]):
            atomic_new(path, content)


def initialize(root, schemas, inventory=None):
    os.umask(0o077)
    root.mkdir(parents=True, exist_ok=True)
    with open(root/'.deploy.lock', 'a') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        shared = root/'shared'
        complete = shared/'bootstrap-complete.json'
        if complete.exists():
            required = [shared/'server.env'] + [shared/'secrets'/n for n in SECRET_FILES]
            if not all(p.is_file() and p.stat().st_size for p in required):
                raise BootstrapError('completed_installation_missing_files_refusing_reinitialize')
            generate_keys(shared/'secrets')
            print('BOOTSTRAP already_initialized=true')
            return
        targets, redis, network = choose_targets(running_containers() if inventory is None else inventory)
        for path in ('releases', 'incoming', 'shared/secrets', 'shared/proxy', 'shared/acme', 'shared/letsencrypt'):
            (root/path).mkdir(parents=True, exist_ok=True)
        state = shared/'bootstrap-state.json'
        planned = {s: {'host': c['Name'].lstrip('/'), 'database': 'ivrcbdc_'+s, 'role': 'ivrcbdc_'+s}
                   for s, c in targets.items()}
        # Existing passwords belong to the running installation. Read exactly as
        # the runtime shell wrapper does, preserve the file bytes, and authenticate
        # every existing secret before any CREATE ROLE/DATABASE or schema change.
        passwords = {}
        for service, container in targets.items():
            path = shared/'secrets'/('db_'+service)
            if not path.exists():
                raise BootstrapError('existing_db_secret_missing_refusing_generation')
            try:
                password = path.read_bytes().decode('utf-8').rstrip('\n')
            except (OSError, UnicodeError):
                raise BootstrapError('existing_db_secret_unreadable') from None
            if not password or any(ord(c) < 32 or ord(c) == 127 for c in password):
                raise BootstrapError('existing_db_secret_empty_or_unsupported_control_character')
            name = planned[service]['role']
            admin = db_client(container)
            existing_role = admin('SELECT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='+literal(name)+');')
            # On resume, a previously created restricted role uses this password.
            # Before role creation, the provided secret authenticates the existing
            # PostgreSQL owner. Never ALTER an existing password to make it fit.
            user = name if existing_role == 't' else None
            try:
                if db_client(container, user=user, password=password)('SELECT 1;') != '1':
                    raise BootstrapError('existing_db_secret_connection_failed')
            except BootstrapError:
                raise BootstrapError('existing_db_secret_connection_failed') from None
            passwords[service] = password
        if passwords:
            print('BOOTSTRAP existing_db_secrets_authenticated=true')
        if state.exists():
            if json.loads(state.read_text()) != planned:
                raise BootstrapError('bootstrap_targets_changed_refusing_reconfigure')
        else:
            if (shared/'server.env').exists():
                raise BootstrapError('existing_server_configuration_refusing_overwrite')
            for service, container in targets.items():
                name = planned[service]['database']
                sql = db_client(container)
                present = sql('SELECT EXISTS(SELECT 1 FROM pg_database WHERE datname='+literal(name)+') '
                              'OR EXISTS(SELECT 1 FROM pg_roles WHERE rolname='+literal(name)+');')
                if present != 'f':
                    raise BootstrapError('existing_app_database_or_role_refusing_adoption')
            atomic_new(state, json.dumps(planned, sort_keys=True)+'\n')
        # Only existing, successfully authenticated passwords are used.
        for service, container in targets.items():
            password = passwords[service]
            name = planned[service]['database']
            admin = db_client(container)
            exists = admin('SELECT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='+literal(name)+');')
            if exists == 'f':
                admin('CREATE ROLE "'+name+'" LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD '+literal(password)+';')
            owner = admin('SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname='+literal(name)+';')
            if not owner:
                admin('CREATE DATABASE "'+name+'" OWNER "'+name+'";')
            elif owner != name:
                raise BootstrapError('existing_database_owner_mismatch')
            app = db_client(container, user=name, password=password, database=name)
            app("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, filename TEXT NOT NULL, applied_at TIMESTAMPTZ NOT NULL DEFAULT now());")
            for filename, body in schemas[service].items():
                version = int(filename.split('_', 1)[0])
                existing = app('SELECT filename FROM schema_migrations WHERE version='+str(version)+';')
                if existing and existing != filename:
                    raise BootstrapError('existing_migration_version_mismatch')
                if existing:
                    continue
                app("BEGIN; SELECT pg_advisory_xact_lock(hashtext('ivr_migrations'));\n"+body+
                    '\nINSERT INTO schema_migrations(version, filename) VALUES ('+str(version)+', '+literal(filename)+'); COMMIT;')
            print('BOOTSTRAP '+service+'_schema_ready=true')
        generate_keys(shared/'secrets')
        config = dict(SHARED_DIR=str(shared), DEPLOY_UID=str(os.getuid()), DEPLOY_GID=str(os.getgid()), APP_NETWORK=network)
        for service, connection in planned.items():
            prefix = service.upper()+'_DB_'
            config.update({prefix+'HOST': connection['host'], prefix+'PORT': '5432',
                           prefix+'NAME': connection['database'], prefix+'USER': connection['role']})
        redis_host = redis['Name'].lstrip('/')
        config.update(REDIS_HOST=redis_host, REDIS_PORT='6379', REDIS_URL='redis://'+redis_host+':6379')
        if not all(re.fullmatch(r'[A-Za-z0-9_./:-]+', v) for v in config.values()):
            raise BootstrapError('unsupported_connection_value')
        atomic_new(shared/'server.env', ''.join(k+'='+v+'\n' for k, v in config.items()))
        fingerprint = hashlib.sha256(json.dumps(schemas, sort_keys=True).encode()).hexdigest()
        atomic_new(shared/'auto-deploy-ready', 'initial-configuration-complete\n')
        atomic_new(complete, json.dumps({'schema_sha256': fingerprint, 'version': 1})+'\n')
        print('BOOTSTRAP application_configuration_ready=true')


if __name__ == '__main__':
    if sys.argv[1:] != ['--apply'] or 'SCHEMAS' not in globals():
        raise SystemExit('Explicit --apply and committed schema payload required')
    try:
        initialize(Path('/home/deploy/ivr-cbdc'), SCHEMAS)
    except BootstrapError as error:
        print('BOOTSTRAP_FAILED '+str(error))
        raise SystemExit(1) from None
    except Exception:
        print('BOOTSTRAP_FAILED unexpected_error_details_suppressed')
        raise SystemExit(1) from None
