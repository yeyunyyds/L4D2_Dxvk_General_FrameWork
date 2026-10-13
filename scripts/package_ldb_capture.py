"""Package only a matching, actually compiled MSVC development trio."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile
from prepare_research import ROOT


def package(source, build_id, output):
    if output.exists():
        raise ValueError('Preserve existing package; choose another --output')
    receipt = json.loads((source / 'capture-source.json').read_text())
    server = source / 'bridge/src/server'
    for filename, field in [('host_capture.h', 'host_capture_sha256'), ('dxvk_capture_shared.h', 'shared_capture_sha256')]:
        if hashlib.sha256((server / filename).read_bytes()).hexdigest() != receipt[field]:
            raise ValueError('Prepared source changed after receipt')
    expected_id = 'ldb-capture-dev+' + receipt['host_capture_sha256'][:16]
    if build_id != expected_id:
        raise ValueError('Mismatched development identity')
    entries = {}
    examples = {}
    targets = [('x86', 'client/d3d9.dll', 'bin/dxvk_d3d9.dll', 0x14c),
               ('x64', 'server/L4D2Bridge64.exe', 'bin/.l4d2bridge/L4D2Bridge64.exe', 0x8664),
               ('x86_server', 'server/L4D2Bridge32.exe', 'bin/.l4d2bridge/L4D2Bridge32.exe', 0x14c)]
    for role, filename, destination, machine in targets:
        build = source / f'bridge/_captureRelease_{role}'
        commands = json.loads((build / 'compile_commands.json').read_text())
        actual = [item['command'] for item in commands if item['file'].endswith('.cpp')]
        if not actual or not all('/O2' in line and 'NDEBUG' in line and 'DEBUGOPT' not in line for line in actual):
            raise ValueError(f'Actual MSVC Release compilation not observed: {role}')
        data = (build / 'src' / filename).read_bytes()
        if data[:2] != b'MZ' or build_id.encode() not in data:
            raise ValueError(f'Invalid PE or missing build identity: {role}')
        offset = struct.unpack_from('<I', data, 60)[0]
        if data[offset:offset+4] != b'PE\0\0' or struct.unpack_from('<H', data, offset+4)[0] != machine:
            raise ValueError(f'Wrong PE architecture: {role}')
        if role == 'x86_server' and not struct.unpack_from('<H', data, offset+22)[0] & 0x20:
            raise ValueError('x86 Host is not LARGEADDRESSAWARE')
        entries[destination] = data
        examples[role] = actual[0]
    receipt.update(native_host_build='compiled_MSVC_Release', build_id=build_id, compile_examples=examples,
                   l4d2_game_capture='unrun', b580_integrated_capture='unrun', gpu_fg_execution='unrun',
                   binaries_sha256={name: hashlib.sha256(data).hexdigest() for name,data in entries.items()})
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items(): archive.writestr(name, data)
        archive.writestr('capture-build.json', json.dumps(receipt, indent=2)+'\n')
        archive.write(ROOT/'docs/research/capture-series.md', 'README.md')
        archive.write(ROOT/'docs/research/ldb-host-capture.md', 'docs/host-capture.md')
        for name in ['analyze_ldb_capture.py','analyze_capture_series.py','reproject_capture.py','capture_math.py']:
            archive.write(ROOT/'scripts'/name,'scripts/'+name)
        archive.write(source/'LICENSE', 'licenses/Remix-LICENSE')
        archive.write(ROOT/'.research/sources/bridge/LICENSE', 'licenses/LDB-LICENSE')
        archive.write(ROOT/'.research/sources/dxvk/LICENSE', 'licenses/DXVK-LICENSE')
        archive.write(ROOT/'.research/sources/vulkan_headers/LICENSE.md', 'licenses/Vulkan-Headers-LICENSE.md')
    print(output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--build-id', required=True)
    p.add_argument('--output', type=Path, default=ROOT/'.research/dist/ldb-host-capture-dev.zip')
    a = p.parse_args()
    try: package(a.source.resolve(), a.build_id, a.output.resolve())
    except (ValueError, OSError, KeyError, struct.error) as e: p.exit(1, f'{e}\n')
