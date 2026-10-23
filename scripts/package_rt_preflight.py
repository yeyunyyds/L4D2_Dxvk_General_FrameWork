"""Build the prerequisite launcher plus hash-addressed, valid dependency ZIPs.

Dependency bundles stay below GitHub's Git file limit. The launcher fetches them
automatically on first use, then verifies every file before running anything.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
import zlib

from rt_preflight import MAX_PART, digest

ROOT = Path(__file__).resolve().parents[1]
PYTHON_URL = 'https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg'
PYTHON_SHA = '0eb85c2dfccccf1b17352de4c397f69194035b7d37149eacc16f1147d93de3b8'
VULKAN_PAYLOAD_URL = ('https://raw.githubusercontent.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/'
                      '75e75334074a15f2a912cc91396be2280d3733ec/downloads/'
                      'rt-prereq-payload-008180112dfe090e53cf769b.zip')
VULKAN_PAYLOAD_SHA = '008180112dfe090e53cf769bbf4f37c170b2b6070e8cbb8e3d96b3817ac4cb30'
VULKAN_DLL_SHA = '31ee4674b814b712aaa9899005e9d8dde9a07a2e2cb6f6004daa7439e4027948'


def pinned_vulkan_loader(payload, output, payload_sha=VULKAN_PAYLOAD_SHA, dll_sha=VULKAN_DLL_SHA):
    if digest(payload) != payload_sha:
        raise ValueError('Pinned Vulkan loader payload SHA256 mismatch')
    with zipfile.ZipFile(payload) as archive:
        if archive.namelist() != ['vulkan-1.dll'] or archive.testzip() is not None:
            raise ValueError('Pinned Vulkan loader payload layout/CRC mismatch')
        data = archive.read('vulkan-1.dll')
    if hashlib.sha256(data).hexdigest() != dll_sha:
        raise ValueError('Pinned Vulkan loader DLL SHA256 mismatch')
    output.write_bytes(data)
    return output


def compressed_size(path):
    compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
    size = 0
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            size += len(compressor.compress(block))
    return size + len(compressor.flush())


def payloads(files, output):
    groups, group, used = [], [], 0
    for name, path in sorted(files.items()):
        size = compressed_size(path) + len(name.encode('utf-8')) * 2 + 1024
        if size > MAX_PART - 4096:
            raise ValueError('Single dependency exceeds Git size budget: ' + name)
        if used + size > MAX_PART - 4096:
            groups.append(group); group, used = [], 0
        group.append((name, path)); used += size
    if group:
        groups.append(group)
    result = []
    for index, group in enumerate(groups):
        path = output / ('payload-' + str(index) + '.tmp')
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for name, source in group:
                archive.write(source, name)
        sha = digest(path)
        final = output / ('rt-prereq-payload-' + sha[:24] + '.zip')
        if path.stat().st_size > MAX_PART:
            raise ValueError('Dependency bundle exceeds Git size budget')
        path.replace(final)
        result.append({'name': final.name, 'sha256': sha, 'size': final.stat().st_size})
    return result


def python_files(nupkg):
    if digest(nupkg) != PYTHON_SHA:
        raise ValueError('CPython NuGet SHA256 mismatch')
    with zipfile.ZipFile(nupkg) as archive:
        for info in archive.infolist():
            if not info.filename.startswith('tools/') or info.is_dir():
                continue
            name = info.filename[len('tools/'):]
            parts = Path(name).parts
            if (parts[0] not in ('Lib', 'DLLs') and '/' in name or
                    parts[0] in ('include', 'libs', 'Scripts', 'Tools') or
                    len(parts) > 1 and parts[:2] in [('Lib', x) for x in
                        ('site-packages', 'test', 'ensurepip', 'idlelib', 'tkinter', 'turtledemo')]):
                continue
            if not name.endswith(('.pyc', '.pdb', '.lib', '.cat')):
                yield 'python/' + name, archive.read(info)


def package(source, output, python_package, vulkan_loader, build_commit, verify=False):
    if output.exists():
        raise FileExistsError('Preserve package output; choose a new directory')
    output.mkdir(parents=True)
    receipt = json.loads((source / 'rt-runtime-source.json').read_text())
    baseline = json.loads((ROOT / 'integration/rt/baseline.json').read_text())
    if receipt['commit'] != baseline['runtime_commit'] or receipt['submodules'] != 'ready':
        raise ValueError('Runtime source receipt differs from pin')
    files = {p.relative_to(source / '_output').as_posix(): p
             for p in (source / '_output').rglob('*')
             if p.is_file() and p.suffix.lower() not in ('.pdb', '.lib', '.exp', '.ilk', '.pyc')}
    files['rt-runtime-smoke.exe'] = source / 'checks/rt-runtime-smoke.exe'
    if digest(vulkan_loader) != VULKAN_DLL_SHA:
        raise ValueError('Pinned Vulkan loader differs from audited product')
    files['vulkan-1.dll'] = vulkan_loader
    if os.name == 'nt':
        vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
                                                 '-property', 'installationPath'], text=True).strip()
        redist = Path(installation) / 'VC/Redist/MSVC'
        choices = sorted(redist.glob('*/x64/Microsoft.VC*.CRT'))
        if not choices:
            raise ValueError('Missing redistributable x64 Visual C++ runtime')
        for path in choices[-1].glob('*.dll'):
            files.setdefault(path.name, path)
    if not files.get('d3d9.dll', Path('missing')).is_file() or not files['rt-runtime-smoke.exe'].is_file():
        raise ValueError('Missing compiled runtime or diagnostic')
    print('Packaging', len(files), 'runtime/dependency files', flush=True)
    parts = payloads(files, output)
    manifest = {'schema': 1, 'scope': 'rt_prerequisites', 'build_commit': build_commit,
                'runtime_commit': receipt['commit'], 'runtime_execution': 'unrun',
                'runtime_patches': {'skinning': {'sha256': receipt['runtime_skinning_patch_sha256'],
                    'source_sha256': receipt['runtime_skinning_source_sha256'],
                    'geometry_source_sha256': receipt['runtime_geometry_source_sha256'],
                    'weight_stride': receipt['runtime_skinning_weight_stride'],
                    'command_counter': receipt['runtime_skinning_command_counter']}},
                'vulkan_loader': {'repository': 'https://github.com/KhronosGroup/Vulkan-Loader.git',
                    'commit': 'fb78607414e154c7a5c01b23177ba719c8a44909',
                    'headers_commit': '409c16be502e39fe70dd6fe2d9ad4842ef2c9a53',
                    'sha256': VULKAN_DLL_SHA, 'scope': 'app_local_standard_loader_not_gpu_driver'},
                'game_hook': 'not_installed', 'source_threading': 'unrun',
                'payloads': parts, 'runtime_files': {name: {'size': path.stat().st_size,
                    'sha256': digest(path)} for name, path in sorted(files.items())},
                'python': {'version': '3.12.10', 'url': PYTHON_URL, 'sha256': PYTHON_SHA}}
    package = output / 'rt-preflight-dev.zip'
    with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in ('rt_preflight.py', 'rt_game_probe.py', 'rt_suite.py', 'rt_visual.py'):
            archive.write(ROOT / 'scripts' / name, 'scripts/' + name)
        for name in ('integration/rt/baseline.json', 'research/rt-source-audit.json',
                     'research/rt-source-queue-sdk.json'):
            archive.write(ROOT / name, name)
        for name, data in python_files(python_package):
            archive.writestr(name, data)
        for name in ('LICENSE', 'LICENSE-MIT'):
            if (source / name).is_file():
                archive.write(source / name, 'licenses/runtime/' + name)
        # Retain redistributable dependency notices shipped with the exact inputs.
        for parent in (source / 'external', source / 'submodules'):
            if not parent.is_dir():
                continue
            for dependency in parent.iterdir():
                if not dependency.is_dir():
                    continue
                for path in dependency.rglob('*'):
                    if (path.is_file() and path.stat().st_size < 1024 * 1024 and
                            any(x in path.name.casefold() for x in ('license', 'notice', 'eula', 'copyright'))):
                        archive.write(path, 'licenses/dependencies/' + path.relative_to(source).as_posix())
        archive.writestr('preflight-manifest.json', json.dumps(manifest, indent=2) + '\n')
        archive.write(ROOT / 'integration/rt/preflight-readme.txt', 'README.txt')
        archive.writestr('run-test.cmd', '@echo off\r\ncd /d "%~dp0"\r\n"%~dp0python\\python.exe" -I "%~dp0scripts\\rt_preflight.py" %*\r\nset "LDB_TEST_RESULT=%errorlevel%"\r\necho.\r\necho Result ZIP is in the results folder.\r\npause\r\nexit /b %LDB_TEST_RESULT%\r\n')
    if package.stat().st_size > MAX_PART:
        raise ValueError('Launcher ZIP exceeds Git size budget')
    if verify:
        with tempfile.TemporaryDirectory(prefix='rt-prereq-verify-') as temporary:
            extracted = Path(temporary)
            with zipfile.ZipFile(package) as archive:
                archive.extractall(extracted)
            # Execute the shipped Python and script, not the CI system Python.
            executed = subprocess.run([str(extracted / 'python/python.exe'), '-I', str(extracted / 'scripts/rt_preflight.py'),
                                       '--verify-package', '--payload-cache', str(output.resolve())], check=False)
            results = list((extracted / 'results').glob('*/summary.json'))
            if len(results) != 1:
                raise ValueError('Missing packaged verification report')
            report = json.loads(results[0].read_text(encoding='utf-8'))
            shutil.copyfile(results[0], output / 'package-validation.json')
            if executed.returncode:
                raise ValueError('Packaged coordinator failed: ' + json.dumps(report['errors']))
            if report.get('runtime_load', {}).get('dependency_load') != 'pass' or report['errors']:
                raise ValueError('Packaged dependency loader check failed')
    index = {'schema': 1, 'commit': build_commit, 'launcher': package.name, 'payloads': parts,
             'package_validation': 'pass' if verify else 'unrun', 'gpu_execution': 'unrun'}
    (output / 'publish-index.json').write_text(json.dumps(index, indent=2) + '\n')
    print(package, flush=True)
    return [package] + [output / part['name'] for part in parts]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '.research/rt-remix-runtime')
    parser.add_argument('--output', type=Path, default=ROOT / '.research/dist/rt-prerequisites')
    parser.add_argument('--python-package', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='rt-cpython-') as temporary:
        nupkg = args.python_package or Path(temporary) / 'python.nupkg'
        if not args.python_package:
            with urllib.request.urlopen(PYTHON_URL, timeout=60) as response, nupkg.open('wb') as target:
                shutil.copyfileobj(response, target)
        vulkan_payload = Path(temporary) / 'vulkan-loader.zip'
        with urllib.request.urlopen(VULKAN_PAYLOAD_URL, timeout=60) as response, vulkan_payload.open('wb') as target:
            shutil.copyfileobj(response, target)
        vulkan_loader = pinned_vulkan_loader(vulkan_payload, Path(temporary) / 'vulkan-1.dll')
        package(args.source.resolve(), args.output.resolve(), nupkg, vulkan_loader,
                os.environ.get('GITHUB_SHA', 'local_uncommitted'), args.verify)
