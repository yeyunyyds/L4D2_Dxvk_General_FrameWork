"""Add a genuine application-local Vulkan loader to the verified native product.

Reuse the compiled runtime, CPython and all dependency ZIPs from its own CI run;
the runtime DLL is never modified or rebuilt by this script.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from rt_preflight import digest, relative

NATIVE_COMMIT = '9c3b2f860095d4e75e283d24dd9a54c0343554ec'
LOADER_COMMIT = 'fb78607414e154c7a5c01b23177ba719c8a44909'
HEADERS_COMMIT = '409c16be502e39fe70dd6fe2d9ad4842ef2c9a53'


def repackage(candidate, loader, loader_source, headers_source, output):
    if output.exists():
        raise FileExistsError('Preserve existing package output')
    output.mkdir(parents=True)
    from runtime_imports import imports
    if imports(loader)['machine'] != '0x8664':
        raise ValueError('Expected x64 Vulkan loader')
    for directory, expected in ((loader_source, LOADER_COMMIT), (headers_source, HEADERS_COMMIT)):
        if subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip() != expected:
            raise ValueError('Vulkan loader/headers checkout differs from pin')
    with zipfile.ZipFile(candidate) as original:
        manifest = json.loads(original.read('preflight-manifest.json'))
        if (manifest['build_commit'] != NATIVE_COMMIT or
                manifest['runtime_files']['d3d9.dll']['sha256'] != '21c826bac9fe6a27b28d0b1300f838efa21b010b24216c44f4728b266d8a8ec7'):
            raise ValueError('Existing runtime product differs from native evidence')
        for part in manifest['payloads']:
            path = candidate.parent / part['name']
            if path.stat().st_size != part['size'] or digest(path) != part['sha256']:
                raise ValueError('Existing dependency ZIP differs from native product')
            shutil.copyfile(path, output / path.name)
        temporary = output / 'loader.zip.tmp'
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.write(loader, 'vulkan-1.dll')
        sha = digest(temporary)
        part = output / ('rt-prereq-payload-' + sha[:24] + '.zip')
        temporary.replace(part)
        manifest['payloads'].append({'name': part.name, 'size': part.stat().st_size, 'sha256': sha})
        manifest['runtime_files']['vulkan-1.dll'] = {'sha256': digest(loader), 'size': loader.stat().st_size}
        manifest.update(build_commit=os.environ['GITHUB_SHA'], native_build_commit=NATIVE_COMMIT,
                        code_commit=NATIVE_COMMIT,
                        vulkan_loader={'repository': 'https://github.com/KhronosGroup/Vulkan-Loader.git',
                                       'commit': LOADER_COMMIT, 'headers_commit': HEADERS_COMMIT,
                                       'scope': 'app_local_standard_loader_not_gpu_driver'})
        primary = output / 'rt-preflight-dev.zip'
        with zipfile.ZipFile(primary, 'w', zipfile.ZIP_DEFLATED) as archive:
            for info in original.infolist():
                relative(info.filename)
                if info.filename != 'preflight-manifest.json':
                    archive.writestr(info, original.read(info))
            archive.writestr('preflight-manifest.json', json.dumps(manifest, indent=2)+'\n')
            archive.write(loader_source / 'LICENSE.txt', 'licenses/vulkan-loader/LICENSE.txt')
            # The loader also contains permissively licensed cJSON/dirent code.
            # Preserve the exact source copyright/license headers with the DLL.
            notices = []
            for path in sorted((loader_source / 'loader').rglob('*')):
                if path.is_file() and path.suffix in ('.c', '.h', '.cpp', '.S', '.asm'):
                    source = path.read_text(encoding='utf-8', errors='replace').lstrip()
                    end = source.find('*/')
                    if source.startswith('/*') and 0 <= end < 16384:
                        notices.append(path.relative_to(loader_source).as_posix() + '\n' + source[:end+2])
            archive.writestr('licenses/vulkan-loader/SOURCE-NOTICES.txt', '\n\n'.join(notices)+'\n')
            archive.write(headers_source / 'LICENSE.md', 'licenses/vulkan-headers/LICENSE.md')
            for path in (headers_source / 'LICENSES').iterdir():
                if path.is_file():
                    archive.write(path, 'licenses/vulkan-headers/LICENSES/' + path.name)
    extracted = ROOT / '.research/rt-package-repaired'
    extracted.mkdir()
    with zipfile.ZipFile(primary) as archive:
        archive.extractall(extracted)
    executed = subprocess.run([str(extracted / 'python/python.exe'), '-I',
                               str(extracted / 'scripts/rt_preflight.py'), '--verify-package',
                               '--payload-cache', str(output.resolve())], check=False, timeout=180)
    reports = list((extracted / 'results').glob('*/summary.json'))
    if len(reports) != 1:
        raise ValueError('Missing packaged native loader verification report')
    report = json.loads(reports[0].read_text(encoding='utf-8'))
    report['vulkan_loader'] = manifest['vulkan_loader'] | manifest['runtime_files']['vulkan-1.dll']
    report['native_build_commit'] = NATIVE_COMMIT
    report['primary_zip_sha256'] = digest(primary)
    (output / 'package-validation.json').write_text(json.dumps(report, indent=2)+'\n')
    if executed.returncode or report['errors'] or report.get('runtime_load', {}).get('dependency_load') != 'pass':
        raise ValueError('Repaired product loader verification failed: ' + json.dumps(report['errors']))
    index = {'schema': 1, 'commit': os.environ['GITHUB_SHA'], 'native_build_commit': NATIVE_COMMIT,
             'launcher': primary.name, 'payloads': manifest['payloads'],
             'package_validation': 'pass', 'gpu_execution': 'unrun'}
    (output / 'publish-index.json').write_text(json.dumps(index, indent=2)+'\n')
    print('Packaged native dependency load/export: PASS; GPU execution unrun')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', type=Path, required=True)
    args = parser.parse_args()
    candidates = list((ROOT / '.research/rt-existing-product').rglob('rt-preflight-dev.zip'))
    if len(candidates) != 1:
        raise SystemExit('Expected exactly one preserved native product')
    repackage(candidates[0], args.loader.resolve(), ROOT / '.research/vulkan-loader',
              ROOT / '.research/vulkan-headers', ROOT / '.research/dist/rt-prerequisites')
