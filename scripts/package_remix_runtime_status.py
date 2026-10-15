"""Preserve custom-runtime build evidence without claiming a usable game package."""

import hashlib
import json
import os
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def x64_pe(data):
    if len(data) < 64 or data[:2] != b'MZ':
        return False
    offset = struct.unpack_from('<I', data, 60)[0]
    return (offset + 6 <= len(data) and data[offset:offset+4] == b'PE\0\0' and
            struct.unpack_from('<H', data, offset+4)[0] == 0x8664)


if __name__ == '__main__':
    source = ROOT / '.research/rt-remix-runtime'
    output = ROOT / '.research/dist/rt-runtime-build-status.zip'
    output.parent.mkdir(parents=True, exist_ok=True)
    status = {'schema': 1, 'commit': os.environ['GITHUB_SHA'],
              'prepare_outcome': os.environ.get('REMIX_PREPARE_OUTCOME', 'unknown'),
              'native_outcome': os.environ.get('REMIX_BUILD_OUTCOME', 'unknown'),
              'artifact_validation': 'unrun', 'static_scene_execution': 'unrun',
              'game_rt': 'unrun', 'b580_rt': 'unrun', 'source_threading': 'unrun',
              'scope': 'Custom runtime build evidence; dependencies/game hook not bundled', 'binaries': {}}
    temporary = output.with_suffix('.zip.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
        for log in sorted((ROOT / 'ci-remix-logs').glob('*.log')):
            archive.write(log, 'logs/' + log.name)
        receipt = source / 'rt-runtime-source.json'
        if receipt.exists():
            archive.write(receipt, receipt.name)
        for path in (source / '_RtRelease/meson-logs/meson-log.txt', source / 'update-deps.log',
                     source / 'LICENSE', source / 'packman-external.xml', source / '_RtRelease/compile_commands.json'):
            if path.exists():
                archive.write(path, 'source/' + str(path.relative_to(source)).replace('\\', '/'))
        for name, path in [('rt-runtime-smoke.exe', source / 'checks/rt-runtime-smoke.exe'),
                           ('d3d9.dll', source / '_RtRelease/src/d3d9/d3d9.dll')]:
            if not path.exists():
                continue
            data = path.read_bytes()
            entry = {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data), 'x64_pe': x64_pe(data)}
            # Preserve a small status ZIP even if the runtime/debug information grows.
            entry['included'] = len(data) <= 80 * 1024 * 1024
            status['binaries'][name] = entry
            if entry['included']:
                archive.write(path, 'development/' + name)
        if status['native_outcome'] == 'success':
            valid = len(status['binaries']) == 2 and all(x['x64_pe'] for x in status['binaries'].values())
            status['artifact_validation'] = 'pass' if valid else 'fail'
        archive.writestr('status.json', json.dumps(status, indent=2) + '\n')
        archive.writestr('README.txt', 'Development evidence only. The custom runtime may require additional DLLs and USD files from the CI artifact.\nNo L4D2 Source hook or combined test package A is included. GPU execution remains unrun.\n')
    temporary.replace(output)
    print(output)
