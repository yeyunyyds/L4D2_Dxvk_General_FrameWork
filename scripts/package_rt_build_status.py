"""Publish native foundation evidence; this is not a playable RT package."""

import hashlib
import json
import os
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    source = ROOT / '.research/rt-observer-source'
    output = ROOT / '.research/dist/rt-foundation-build-status.zip'
    output.parent.mkdir(parents=True, exist_ok=True)
    status = {'schema': 1, 'commit': os.environ['GITHUB_SHA'],
              'native_outcome': os.environ.get('RT_BUILD_OUTCOME', 'unknown'),
              'scope': 'Bridge + COM/protocol/Host foundation, not game hooks or GPU RT',
              'game_queue_execution': 'unrun', 'b580_rt': 'unrun', 'binaries': {},
              'sdk_queue_development_execution': 'unrun',
              'artifact_validation': 'unrun'}
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted((ROOT / 'ci-rt-logs').glob('*.log')):
            archive.write(file, 'logs/' + file.name)
        receipt = source / 'rt-source.json'
        identity = None
        if receipt.exists():
            archive.write(receipt, 'rt-source.json')
            identity = ('ldb-rt-observer+' + json.loads(receipt.read_text())['fingerprint'][:16]).encode()
        sdk_receipt = ROOT / '.research/rt-sdk-source/rt-sdk-source.json'
        if sdk_receipt.exists():
            archive.write(sdk_receipt, 'rt-sdk-source.json')
        queue_source = ROOT / 'integration/rt/rt_source_queue.h'
        if queue_source.exists():
            status['source_queue_sha256'] = hashlib.sha256(queue_source.read_bytes()).hexdigest()
        for arch, relative in [('x64', 'server/L4D2Bridge64.exe'), ('x86', 'client/d3d9.dll')]:
            file = source / f'bridge/_rtRelease_{arch}/src' / relative
            if file.exists():
                data = file.read_bytes()
                if status['native_outcome'] == 'success':
                    if len(data) < 64 or data[:2] != b'MZ' or not identity or identity not in data:
                        raise RuntimeError('Missing PE/compiled identity: ' + str(file))
                    offset = struct.unpack_from('<I', data, 60)[0]
                    expected_machine = 0x8664 if arch == 'x64' else 0x14c
                    if data[offset:offset+4] != b'PE\0\0' or struct.unpack_from('<H', data, offset+4)[0] != expected_machine:
                        raise RuntimeError('Wrong native architecture: ' + str(file))
                    commands = file.parents[2] / 'compile_commands.json'
                    actual = [x['command'] for x in json.loads(commands.read_text()) if x['file'].endswith('.cpp')]
                    if not actual or not all('/O2' in x and 'NDEBUG' in x and 'DEBUGOPT' not in x for x in actual):
                        raise RuntimeError('Release compiler commands not observed: ' + arch)
                    archive.write(commands, 'development/' + arch + '/compile_commands.json')
                destination = 'development/' + arch + '/' + file.name
                status['binaries'][destination] = hashlib.sha256(data).hexdigest()
                archive.write(file, destination)
            for file in sorted((source / f'checks-{arch}').rglob('*')):
                if file.is_file() and file.suffix in ('.exe', '.bin', '.asm', '.json'):
                    archive.write(file, 'checks/' + arch + '/' + str(file.relative_to(source / f'checks-{arch}')))
        if status['native_outcome'] == 'success':
            if len(status['binaries']) != 2:
                raise RuntimeError('Missing native client/Host binary')
            status['artifact_validation'] = 'pass'
            queue_log = ROOT / 'ci-rt-logs/native.log'
            if sdk_receipt.exists() and queue_log.exists() and 'RT L4D2 SDK queue/COM/Host: PASS' in queue_log.read_text(errors='replace'):
                status['sdk_queue_development_execution'] = 'pass'
        archive.writestr('status.json', json.dumps(status, indent=2) + '\n')
        archive.writestr('README.txt', 'Development build evidence only. No Source game hook or Remix RT runtime is included.\nDo not install this ZIP as combined test package A.\n')
    print(output)
