#!/usr/bin/env python3
"""Read-only bootstrap capabilities; stdout is a fixed set of booleans only.
Never log names, images, topology, connection values, filesystem paths or secrets.
"""
import json
from pathlib import Path
import subprocess


def command(args):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=15)
        return result.returncode, result.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ''


def active(name):
    return command(['systemctl', 'is-active', '--quiet', name])[0] == 0


def inspect():
    flags = dict.fromkeys(['postgres_in_docker', 'redis_in_docker', 'postgres_on_host',
                          'redis_on_host', 'k3s_active', 'sudo_without_password',
                          'app_db_credentials_present', 'jwt_mounts_present',
                          'postgres_bootstrap_credentials_present', 'registry_login_present'], False)
    flags.update(postgres_on_host=active('postgresql'), redis_on_host=active('redis-server'),
                 k3s_active=active('k3s'), sudo_without_password=command(['sudo', '-n', 'true'])[0] == 0)
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
