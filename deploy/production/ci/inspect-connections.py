#!/usr/bin/env python3
"""Read-only bootstrap capabilities; stdout is a fixed set of booleans only.
Never log names, images, topology, connection values, filesystem paths or secrets.
"""
import json
import os
import shutil
from pathlib import Path
import subprocess


def command(args, env=None):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=15, env=env)
        return result.returncode, result.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ''


def active(name):
    return command(['systemctl', 'is-active', '--quiet', name])[0] == 0


def inspect():
    flags = dict.fromkeys(['postgres_in_docker', 'redis_in_docker', 'postgres_on_host',
                          'redis_on_host', 'k3s_active', 'sudo_without_password',
                          'app_db_credentials_present', 'jwt_mounts_present',
                          'postgres_bootstrap_credentials_present', 'registry_login_present', 'postgres_catalog_readable',
                          'auth_schema_present', 'core_schema_present', 'commission_schema_present',
                          'existing_shared_network', 'redis_compatible', 'openssl_available'], False)
    flags.update(postgres_on_host=active('postgresql'), redis_on_host=active('redis-server'),
                 k3s_active=active('k3s'), sudo_without_password=command(['sudo', '-n', 'true'])[0] == 0)
    flags['openssl_available'] = shutil.which('openssl') is not None
    data_networks = []
    code, ids = command(['docker', 'ps', '-q'])
    if code == 0 and ids.split():
        code, raw = command(['docker', 'inspect', *ids.split()])
        if code == 0:
            for item in json.loads(raw):
                image = item.get('Config', {}).get('Image', '').lower()
                env = dict(v.split('=', 1) for v in item.get('Config', {}).get('Env', []) if '=' in v)
                flags['postgres_in_docker'] |= 'postgres' in image
                flags['redis_in_docker'] |= 'redis' in image
                flags['app_db_credentials_present'] |= bool(env.get('DB_PASSWORD') or env.get('PG_DSN'))
                flags['postgres_bootstrap_credentials_present'] |= bool(env.get('POSTGRES_PASSWORD') or env.get('POSTGRES_PASSWORD_FILE'))
                flags['jwt_mounts_present'] |= any('jwt' in m.get('Destination', '').lower() for m in item.get('Mounts', []))
                if 'postgres' in image or 'redis' in image:
                    data_networks.append(set(item.get('NetworkSettings', {}).get('Networks', {})) - {'bridge', 'host', 'none'})
                if 'redis' in image:
                    status, pong = command(['docker', 'exec', item['Id'], 'redis-cli', '--raw', 'PING'])
                    flags['redis_compatible'] |= status == 0 and pong.strip() == 'PONG'
                if 'postgres' in image:
                    password = env.get('POSTGRES_PASSWORD', '')
                    if not password and env.get('POSTGRES_PASSWORD_FILE'):
                        status, password = command(['docker', 'exec', item['Id'], 'cat', env['POSTGRES_PASSWORD_FILE']])
                        password = password.rstrip('\n') if status == 0 else ''
                    # All SQL is SELECT, with read-only transactions enforced by PostgreSQL.
                    client_env = dict(os.environ, PGPASSWORD=password,
                                      PGOPTIONS='-c default_transaction_read_only=on')
                    base = ['docker', 'exec', '-e', 'PGPASSWORD', '-e', 'PGOPTIONS', item['Id'],
                            'psql', '-X', '-w', '-A', '-t', '-U', env.get('POSTGRES_USER', 'postgres')]
                    status, names = command(base + ['-d', 'postgres', '-c',
                         'SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate'], env=client_env)
                    flags['postgres_catalog_readable'] |= status == 0
                    for name in names.splitlines() if status == 0 else []:
                        status, result = command(base + ['-d', name, '-c',
                            "SELECT table_name FROM information_schema.tables WHERE table_schema='public'"], env=client_env)
                        tables = set(result.splitlines()) if status == 0 else set()
                        flags['auth_schema_present'] |= {'users', 'companies'} <= tables
                        flags['core_schema_present'] |= {'deals', 'deal_documents', 'timeline_events', 'notifications', 'outbox', 'document_kinds'} <= tables
                        flags['commission_schema_present'] |= {'corridors', 'scenario_profiles'} <= tables
    flags['existing_shared_network'] = bool(data_networks and set.intersection(*data_networks))

    registry = Path.home()/'.docker/config.json'
    if registry.is_file():
        config = json.loads(registry.read_text())
        flags['registry_login_present'] = bool(config.get('auths', {}).get('ghcr.io') or config.get('credsStore') or config.get('credHelpers', {}).get('ghcr.io'))
    for key, value in flags.items():
        print('BOOTSTRAP_CHECK '+key+'='+str(bool(value)).lower())


if __name__ == '__main__':
    try:
        inspect()
    except Exception:
        # Do not expose exception strings: they can include inspected data.
        print('BOOTSTRAP_CHECK inspection_complete=false')
        raise SystemExit(1)
