#!/usr/bin/env python3
"""Exercise the real SSH script body with stdin-draining fake app/TLS commands.
No SSH, Docker, production files, cron entries or network are accessed.
"""
import hashlib
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile

prod=Path(__file__).resolve().parents[1]
script=(prod/'ci/ssh-deploy.sh').read_text().split("<<'REMOTE'\n",1)[1].rsplit('\nREMOTE',1)[0]
with tempfile.TemporaryDirectory(prefix='ivr-ssh-check-') as temporary:
    root=Path(temporary)
    release_id='test'
    incoming=root/'incoming'/release_id
    incoming.mkdir(parents=True)
    (root/'releases').mkdir()
    (root/'shared').mkdir()
    (root/'shared/auto-deploy-ready').touch()
    files={
        'scripts/deploy.sh': '''#!/bin/bash
set -euo pipefail
cat >/dev/null
pwd -P >/dev/null
ln -s "$2" ../../current
printf 'deploy\\n' >> ../../executed
''',
        'scripts/tls.sh': '''#!/bin/bash
set -euo pipefail
cat >/dev/null
printf 'tls\\n' >> executed
touch shared/tls-enabled
''',
        'scripts/renewal-cron.sh': '''#!/bin/bash
set -euo pipefail
cat >/dev/null
printf 'renewal\\n' >> executed
''',
    }
    # Child deployment starts in root, as all real helpers should.
    files['scripts/deploy.sh']=files['scripts/deploy.sh'].replace('../../current','current').replace('../../executed','executed')
    archive=incoming/'release.tar.gz'
    with tarfile.open(archive,'w:gz') as tar:
        for name,body in files.items():
            data=body.encode()
            info=tarfile.TarInfo(name)
            info.size=len(data)
            info.mode=0o644
            tar.addfile(info,io.BytesIO(data))
    (incoming/'release.tar.gz.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  release.tar.gz\n')
    result=subprocess.run(['bash','-s','--',release_id],input=script.replace('/home/deploy/ivr-cbdc',str(root)),text=True,capture_output=True)
    assert result.returncode==0, result.stderr
    assert (root/'executed').read_text()=='deploy\ntls\nrenewal\n', result.stdout
    assert 'DEPLOYMENT_COMPLETE application_https_and_renewal_ready=true' in result.stdout
    assert not incoming.exists()
print('PASS: SSH child stdin isolation; valid working directory after unpack; TLS and renewal both execute; final completion marker')
