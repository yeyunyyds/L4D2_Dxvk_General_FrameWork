"""Retain fast native collector checks, including partial failure evidence."""

import hashlib
import json
import os
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]


if __name__ == '__main__':
    output = ROOT / '.research/dist/rt-game-probe-build-status.zip'
    output.parent.mkdir(parents=True, exist_ok=True)
    status = {'schema': 1, 'commit': os.environ['GITHUB_SHA'],
              'scope': 'external_x64_reader_x86_fixture_not_game_binding',
              'native_outcome': os.environ.get('RT_PROBE_OUTCOME', 'unknown'),
              'game_queue_execution': 'unrun', 'b580_rt': 'unrun', 'files': {}}
    temporary = output.with_suffix('.zip.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in ('scripts/rt_game_probe.py', 'tests/test_rt_game_probe.py',
                     'tests/rt_game_probe_fixture.cpp'):
            status['files'][name] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        for file in sorted((ROOT / 'ci-game-probe').glob('*')):
            if file.is_file() and file.suffix in ('.log', '.json', '.exe'):
                archive.write(file, file.name)
                status['files'][file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
        archive.writestr('status.json', json.dumps(status, indent=2) + '\n')
        archive.writestr('README.txt', 'Development collector checks only. No game hooks, RT runtime or user test package A.\n')
    temporary.replace(output)
    print(output)
