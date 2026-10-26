#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess
import tempfile
p=Path(__file__).resolve().parents[1]/'scripts/plan.py'
spec=importlib.util.spec_from_file_location('build_plan',p)
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
with tempfile.TemporaryDirectory(prefix='ivr-plan-check-') as td:
    root=Path(td)
    def git(*args): return subprocess.check_output(['git','-C',str(root),*args],stderr=subprocess.DEVNULL)
    git('init');git('config','user.name','Test');git('config','user.email','test@example.invalid')
    paths=['infra/docker/cpp-base.Dockerfile','infra/entrypoint.sh','libs/common/common.h','.dockerignore','deploy/production/image-revision']+[f'services/service-{s}/Dockerfile' for s in ['auth','core','commission']]
    for name in paths:
        f=root/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(name)
    git('add','.');git('commit','-qm','fixture')
    base,old=mod.image_refs(root)
    # Working-tree edits are not input to a main-commit build.
    (root/'services/service-auth/Dockerfile').write_text('local-only edit')
    (root/'.env').write_text('LOCAL_SECRET_MUST_NOT_AFFECT_BUILD')
    assert mod.image_refs(root)==(base,old)
    git('checkout','--','services/service-auth/Dockerfile')
    f=root/'services/service-commission/Dockerfile';f.write_text('changed commission')
    git('add',str(f));git('commit','-qm','fixture')
    base2,new=mod.image_refs(root)
    assert base2==base
    assert new['COMMISSION_IMAGE']!=old['COMMISSION_IMAGE']
    assert new['AUTH_IMAGE']==old['AUTH_IMAGE'] and new['CORE_IMAGE']==old['CORE_IMAGE']
    f=root/'libs/common/common.h';f.write_text('changed common library')
    git('add',str(f));git('commit','-qm','fixture')
    _,shared=mod.image_refs(root)
    assert shared['AUTH_IMAGE']!=new['AUTH_IMAGE'] and shared['CORE_IMAGE']!=new['CORE_IMAGE']
    assert shared['COMMISSION_IMAGE']==new['COMMISSION_IMAGE']
    f=root/'infra/docker/cpp-base.Dockerfile';f.write_text('changed base')
    git('add',str(f));git('commit','-qm','fixture')
    base3,changed=mod.image_refs(root)
    assert base3!=base2
    assert changed['AUTH_IMAGE']!=shared['AUTH_IMAGE'] and changed['CORE_IMAGE']!=shared['CORE_IMAGE']
    assert changed['COMMISSION_IMAGE']==shared['COMMISSION_IMAGE']
print('PASS: selective image fingerprints; shared C++ dependency invalidation; uncommitted local files excluded')
