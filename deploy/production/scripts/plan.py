#!/usr/bin/env python3
"""Content tags use committed build inputs, never local/untracked files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

SERVICES = {'AUTH_IMAGE':'service-auth','CORE_IMAGE':'service-core','COMMISSION_IMAGE':'service-commission'}

def fingerprint(repo, paths, extra=''):
    tree=subprocess.check_output(['git','-C',str(repo),'ls-tree','-r','--full-tree','HEAD','--',*paths])
    if not tree: raise ValueError('No tracked build inputs')
    return hashlib.sha256(b'ivr-build-v1\0'+extra.encode()+b'\0'+tree).hexdigest()

def image_refs(repo):
    common=['.dockerignore','deploy/production/image-revision']
    base='ghcr.io/ivr-cbdc/backend/ivr-cpp-base:src-'+fingerprint(repo,['infra/docker','deploy/production/image-revision'])
    refs={}
    for key,name in SERVICES.items():
        paths=common+['services/'+name]
        extra=''
        if name!='service-commission':
            paths+=['libs/common','infra/entrypoint.sh']
            extra=base
        refs[key]='ghcr.io/ivr-cbdc/backend/'+name+':src-'+fingerprint(repo,paths,extra)
    return base,refs

def exists(ref):
    return subprocess.run(['docker','buildx','imagetools','inspect',ref],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--repo',type=Path,default=Path('.'))
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    base,refs=image_refs(args.repo)
    matrix=[{'service':SERVICES[k],'image':ref} for k,ref in refs.items() if not exists(ref)]
    base_needed=any(v['service']!='service-commission' for v in matrix) and not exists(base)
    args.output.write_text(json.dumps({'base_image':base,'images':refs},indent=2)+'\n')
    values={'matrix':json.dumps({'include':matrix},separators=(',',':')),'base_image':base,'base_needed':str(base_needed).lower(),'build_needed':str(bool(matrix)).lower()}
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'],'a') as f:
            for k,v in values.items(): f.write(k+'='+v+'\n')
    print('Images to build:',', '.join(v['service'] for v in matrix) or 'none (committed inputs unchanged)')

if __name__=='__main__': main()
