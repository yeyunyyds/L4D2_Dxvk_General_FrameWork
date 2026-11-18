"""Package only a matching, actually compiled MSVC development trio."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile
from prepare_research import ROOT
from fetch_fsr_assets import obtain_assets
from fetch_dxvk_runtime import obtain_dxvk_runtime


def validate_package_paths(entries):
    """Keep the user-facing archive extract-and-run: bin/ plus README only."""
    if not entries:
        raise ValueError('Empty package payload')
    for name in entries:
        # Every package payload must stay below bin/. README is added separately.
        if not name.startswith('bin/') or '\\' in name or name.endswith('/'):
            raise ValueError(f'Non-runtime package path: {name}')


def package(source, build_id, output):
    if output.exists():
        raise ValueError('Preserve existing package; choose another --output')
    receipt = json.loads((source / 'capture-source.json').read_text())
    server = source / 'bridge/src/server'
    for filename, field in [('host_capture.h', 'host_capture_sha256'), ('dxvk_capture_shared.h', 'shared_capture_sha256')]:
        if hashlib.sha256((server / filename).read_bytes()).hexdigest() != receipt[field]:
            raise ValueError('Prepared source changed after receipt')
    for name,expected in receipt.get('gpu_sources_sha256',{}).items():
        if hashlib.sha256((server/name).read_bytes()).hexdigest()!=expected:raise ValueError('GPU source changed after receipt')
    expected_id = 'ldb-capture-dev+' + receipt['build_fingerprint'][:16]
    if build_id != expected_id:
        raise ValueError('Mismatched development identity')
    entries = {}
    asset_lock,assets=obtain_assets()
    entries.update({name:data for name,data in assets.items()
                    if name.startswith('bin/') and
                    name != 'bin/.l4d2bridge/d3d9vk_x64.dll'})
    dxvk_lock, dxvk_runtime = obtain_dxvk_runtime()
    entries['bin/.l4d2bridge/d3d9vk_x64.dll'] = dxvk_runtime
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
    layer = ROOT / '.research/asteria-x64-build'
    for filename in ('AsteriaFrameGenerationLayer64.dll',
                     'VK_LAYER_GF_Asteria_FrameGeneration.json',
                     'gf-asteria-layer-build.json'):
        path = layer / filename
        if not path.is_file():
            raise ValueError(f'Missing compiled x64 Vulkan layer file: {filename}')
        entries[f'bin/.l4d2bridge/vulkan-layer/{filename}'] = path.read_bytes()
    entries['bin/.l4d2bridge/fg-config.ini'] = (ROOT/'integration/ldb/fg-config.ini').read_bytes()
    entries['bin/.l4d2bridge/fg_live_hud.ps1'] = (ROOT/'scripts/fg_live_hud.ps1').read_bytes()
    receipt.update(fsr_assets_archive_sha256=asset_lock['archive_sha256'],
                   dxvk_runtime_version=dxvk_lock['version'],
                   dxvk_runtime_archive_sha256=dxvk_lock['archive_sha256'],
                   dxvk_runtime_origin='unmodified_upstream_release',
                   dxvk_private_exports_required=False,
                   fsr_assets_build='FSR backend cross_compiled_MinGW_x64; official DXVK release; Host trio native MSVC',
                   native_host_build='compiled_MSVC_Release', build_id=build_id, compile_examples=examples,
                   l4d2_game_capture='unrun', b580_integrated_capture='unrun', gpu_fg_execution='unrun_in_game', frame_generation_prototype='compiled_with_real_present_path',
                   binaries_sha256={name: hashlib.sha256(data).hexdigest() for name,data in entries.items()})
    entries['bin/.l4d2bridge/capture-build.json'] = (
        json.dumps(receipt, indent=2) + '\n').encode()
    for name, data in assets.items():
        if name.startswith('licenses/'):
            entries['bin/.l4d2bridge/' + name] = data
    licenses = {
        'Remix-LICENSE': source / 'LICENSE',
        'LDB-LICENSE': ROOT / '.research/sources/bridge-134/LICENSE',
        'DXVK-LICENSE': ROOT / '.research/sources/dxvk/LICENSE',
        'Vulkan-Headers-LICENSE.md':
            ROOT / '.research/sources/vulkan_headers/LICENSE.md',
        'Asteria-LICENSE': ROOT / '.research/sources/asteria/LICENSE',
    }
    for name, path in licenses.items():
        entries[f'bin/.l4d2bridge/licenses/{name}'] = path.read_bytes()
    validate_package_paths(entries)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items(): archive.writestr(name, data)
        archive.write(ROOT/'docs/package-readme.md', 'README.md')
    print(output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--build-id', required=True)
    p.add_argument('--output', type=Path, default=ROOT/'.research/dist/ldb-host-capture-dev.zip')
    a = p.parse_args()
    try: package(a.source.resolve(), a.build_id, a.output.resolve())
    except (ValueError, OSError, KeyError, struct.error) as e: p.exit(1, f'{e}\n')
