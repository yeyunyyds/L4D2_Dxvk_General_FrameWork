"""Repack the pinned FSR backend with a newly built, source-matched DXVK."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from prepare_research import ROOT


PROJECT_SOURCES = [
    'tools/fsr_backend_api.h', 'tools/fsr_backend.cpp',
    'tools/fsr_component_backend.h', 'tools/fsr_sdk_support.cpp',
    'tools/fsr_sdk_portability.h', 'tools/fsr_presenter_api.h',
    'integration/ldb/fsr_camera.h', 'integration/ldb/camera_state.h',
    'shaders/fsr_inputs.comp', 'shaders/fsr_compose.comp',
    'scripts/build_fsr_vulkan.py', 'scripts/build_fsr_backend.py',
    'scripts/build_fsr_dxvk.py', 'scripts/package_fsr_backend_assets.py',
    'research/sources.lock.json',
]
EXPECTED_MEMBERS = {
    'bin/.l4d2bridge/LDBFsrBackend64.dll',
    'bin/.l4d2bridge/d3d9vk_x64.dll',
    'fsr-backend-build.json',
    'licenses/FSR-DXVK-LICENSE',
    'licenses/FidelityFX-LICENSE.txt',
    'licenses/MinGW-GCC-copyright',
    'licenses/MinGW-w64-copyright',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main(args):
    with zipfile.ZipFile(args.base) as archive:
        if set(archive.namelist()) != EXPECTED_MEMBERS or archive.testzip():
            raise RuntimeError('Base FSR asset archive is incomplete')
        files = {name: archive.read(name) for name in EXPECTED_MEMBERS}
    metadata = json.loads(files['fsr-backend-build.json'])
    dxvk = json.loads(args.dxvk_build.read_text())
    dll = args.dxvk_dll.read_bytes()
    if digest(dll) != dxvk['dll_sha256']:
        raise RuntimeError('DXVK DLL differs from its build receipt')
    metadata['project_source_hashes'] = {
        name: digest((ROOT / name).read_bytes()) for name in PROJECT_SOURCES
    }
    metadata['dxvk'] = dxvk
    metadata['input_contract'] = {
        'version': 1,
        'header_sha256': digest((ROOT / 'tools/fsr_presenter_api.h').read_bytes()),
        'scope': 'device and queue-family topology plus color/depth/camera/HUD-less frame packet; replacement swapchain not active',
    }
    metadata['game_and_b580_validation'] = 'unrun'
    files['bin/.l4d2bridge/d3d9vk_x64.dll'] = dll
    files['fsr-backend-build.json'] = (
        json.dumps(metadata, indent=2, sort_keys=True) + '\n').encode()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, files[name], compresslevel=9)
    with zipfile.ZipFile(args.output) as archive:
        if set(archive.namelist()) != EXPECTED_MEMBERS or archive.testzip():
            raise RuntimeError('Generated FSR asset archive failed validation')
    print(args.output)
    print(digest(args.output.read_bytes()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--dxvk-dll', type=Path,
                        default=ROOT / '.research/fsr-dxvk/build/src/d3d9/d3d9.dll')
    parser.add_argument('--dxvk-build', type=Path,
                        default=ROOT / '.research/fsr-dxvk/build.json')
    parser.add_argument('--output', type=Path,
                        default=ROOT / '.research/dist/fsr-backend-windows-x64.zip')
    values = parser.parse_args()
    for field in ('base', 'dxvk_dll', 'dxvk_build', 'output'):
        setattr(values, field, getattr(values, field).resolve())
    main(values)
