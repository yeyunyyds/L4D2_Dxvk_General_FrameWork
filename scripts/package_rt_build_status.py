"""Publish native foundation evidence; this is not a playable RT package."""

import hashlib
import json
import os
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    source = ROOT / '.research/rt-observer-source'
    output = ROOT / '.research/dist/rt-foundation-build-status.zip'
    output.parent.mkdir(parents=True, exist_ok=True)
    status = {'schema': 1, 'commit': os.environ['GITHUB_SHA'],
              'native_outcome': os.environ.get('RT_BUILD_OUTCOME', 'unknown'),
              'scope': 'Bridge + COM/protocol/Host foundation, not game hooks or GPU RT',
              'game_queue_execution': 'unrun', 'b580_rt': 'unrun', 'binaries': {}}
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted((ROOT / 'ci-rt-logs').glob('*.log')):
            archive.write(file, 'logs/' + file.name)
        receipt = source / 'rt-source.json'
        if receipt.exists():
            archive.write(receipt, 'rt-source.json')
        for arch, relative in [('x64', 'server/L4D2Bridge64.exe'), ('x86', 'client/d3d9.dll')]:
            file = source / f'bridge/_rtRelease_{arch}/src' / relative
            if file.exists():
                destination = 'development/' + arch + '/' + file.name
                status['binaries'][destination] = hashlib.sha256(file.read_bytes()).hexdigest()
                archive.write(file, destination)
            for file in sorted((source / f'checks-{arch}').rglob('*')):
                if file.is_file() and file.suffix in ('.exe', '.bin'):
                    archive.write(file, 'checks/' + arch + '/' + str(file.relative_to(source / f'checks-{arch}')))
        archive.writestr('status.json', json.dumps(status, indent=2) + '\n')
        archive.writestr('README.txt', 'Development build evidence only. No Source game hook or Remix RT runtime is included.\nDo not install this ZIP as combined test package A.\n')
    print(output)
