#!/usr/bin/env python3
"""Isolated local component checks. No ports, application startup, ACME or registry pushes."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--frontend-image',required=True)
parser.add_argument('--commission-image',required=True)
parser.add_argument('--auth-image',required=True)
args=parser.parse_args()
prod=Path(__file__).resolve().parents[1]

def execute(cmd, success=True):
    r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if success and r.returncode: raise AssertionError(r.stdout+'\n'+r.stderr)
    if not success and r.returncode==0: raise AssertionError('Expected refusal')
    return r

def bind(src,dst): return ['--mount',f'type=bind,source={src},target={dst},readonly']
base=['docker','run','--rm','--pull','never','--network','none','--memory','128m','--pids-limit','64']
with tempfile.TemporaryDirectory(prefix='ivr-prod-runtime-check-') as td:
    root=Path(td)
    conf=root/'conf'; conf.mkdir()
    cert=root/'certs/live/alfacbcd.ru'; cert.mkdir(parents=True)
    acme=root/'acme'; acme.mkdir()
    execute(['openssl','req','-x509','-nodes','-newkey','rsa:2048','-days','1','-subj','/CN=alfacbcd.ru','-keyout',str(cert/'privkey.pem'),'-out',str(cert/'fullchain.pem')])
    for mode in ['http','https']:
        shutil.copy2(prod/f'proxy/{mode}.conf',conf/'default.conf')
        cmd=base+['--user','0:0','--entrypoint','nginx']+bind(conf,'/etc/nginx/conf.d')+bind(root/'certs','/etc/letsencrypt')+bind(acme,'/var/www/acme')+[args.frontend_image,'-t']
        execute(cmd)
    secrets=root/'secrets'; secrets.mkdir()
    password='synthetic pass:@/\\"$test'
    secret=secrets/'db_password'; secret.write_text(password); secret.chmod(0o600)
    script=prod/'scripts/secrets-entrypoint.sh'
    common=base+['--user',f'{os.getuid()}:{os.getgid()}','--entrypoint','/bin/sh','-e','DB_NAME=existing/db','-e','DB_USER=user@prod','-e','DB_HOST=pg-commission','-e','DB_PORT=5432']+bind(secret,'/run/secrets/db_password')+bind(script,'/wrapper.sh')
    execute(common+[args.commission_image,'/wrapper.sh','python','-c',"import os; from urllib.parse import quote; assert os.environ['PG_DSN']=='postgresql+asyncpg://user%40prod:'+quote(os.environ['DB_PASSWORD'],safe='')+'@pg-commission:5432/existing%2Fdb'"])
    # C++ values are substituted into JSON: preserve special characters by escaping.
    result=execute(common+[args.auth_image,'/wrapper.sh','printenv','DB_PASSWORD'])
    assert result.stdout.rstrip('\n')==password.replace('\\','\\\\').replace('"','\\"'), repr(result.stdout)
    assert secret.read_text()==password
    secret.write_text('')
    execute(common+[args.commission_image,'/wrapper.sh','true'],success=False)
print('PASS: HTTP/HTTPS nginx configuration; non-root existing-secret readability; Python URL/C++ JSON escaping; empty secret refusal. No network/ACME/database access.')
