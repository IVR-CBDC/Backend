#!/usr/bin/env python3
"""One-time SQL/source payload, separate from the application release archive."""
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
files = {'auth': ['001_init.sql', '002_company.sql'],
         'core': ['001_init.sql', '002_deals.sql', '003_document_attempts.sql'],
         'commission': ['001_corridors.sql', '002_scenario_profiles.sql', '003_seed.sql']}
schemas = {service: {name: subprocess.check_output(
    ['git', '-C', str(repo), 'show', f'HEAD:services/service-{service}/migrations/{name}'], text=True)
    for name in names} for service, names in files.items()}
source = (repo/'deploy/production/ci/bootstrap-host.py').read_text()
output = 'SCHEMAS = '+repr(schemas)+'\n'+source
compile(output, 'production-bootstrap', 'exec')
Path(sys.argv[1]).write_text(output)
print('Bootstrap payload: committed schema/reference SQL only; no user data, dumps or credentials')
