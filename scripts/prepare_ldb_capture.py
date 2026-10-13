"""Prepare an isolated pinned LDB source tree with Host capture hooks."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

from prepare_research import ROOT, require_source
from build_host_capture import generate_interop


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checkout(path, repository, commit, sparse=None):
    if path.exists():
        return
    path.mkdir(parents=True)
    subprocess.run(['git', 'init', '-q', str(path)], check=True)
    subprocess.run(['git', 'remote', 'add', 'origin', repository], cwd=path, check=True)
    subprocess.run(['git', 'fetch', '--depth=1', *(['--filter=blob:none'] if sparse else []), 'origin', commit], cwd=path, check=True)
    if sparse:
        subprocess.run(['git', 'sparse-checkout', 'set', *sparse], cwd=path, check=True)
    subprocess.run(['git', 'checkout', '--detach', 'FETCH_HEAD'], cwd=path, check=True)


def prepare(args):
    baseline = json.loads((ROOT / 'integration/ldb/baseline.json').read_text())
    lock = json.loads((ROOT / 'research/sources.lock.json').read_text())
    if args.output.exists():
        raise RuntimeError('Output already exists; choose a new isolated --output directory')
    checkout(args.bridge, baseline['bridge_repository'], baseline['bridge_commit'])
    checkout(args.upstream, baseline['upstream_repository'], baseline['upstream_commit'], ['bridge', 'public'])
    detours = ROOT / '.research/sources/detours'
    checkout(detours, baseline['detours_repository'], baseline['detours_commit'])
    require_source(detours, baseline['detours_commit'])
    require_source(args.bridge, baseline['bridge_commit'])
    # git archive reads the pinned commit, never local upstream modifications.
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=args.upstream, text=True).strip()
    if head != baseline['upstream_commit']:
        raise RuntimeError('Wrong upstream commit; checkout preserved')
    require_source(args.dxvk, lock['sources']['dxvk']['commit'])
    require_source(args.headers, lock['sources']['vulkan_headers']['commit'])
    patch = args.bridge / 'patches/l4d2-bridge.patch'
    if digest(patch) != baseline['base_patch_sha256']:
        raise RuntimeError('LDB baseline patch hash differs')
    archive = subprocess.check_output(['git', 'archive', 'HEAD', 'bridge', 'public', 'LICENSE',
                                       'src/util/util_per_game_config.h', 'src/util/util_crash_report.h', 'src/util/xxHash'], cwd=args.upstream)
    args.output.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(args.output, filter='data')
    subprocess.run(['git', 'init', '-q', str(args.output)], check=True)
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=args.output, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=args.output, check=True)
    server = args.output / 'bridge/src/server'
    if digest(server / 'main.cpp') != baseline['post_base_main_sha256']:
        raise RuntimeError('Post-baseline Host differs from reviewed dispatcher')
    hooks = ROOT / 'integration/ldb/host-hooks.patch'
    subprocess.run(['git', 'apply', '--check', str(hooks)], cwd=args.output, check=True)
    subprocess.run(['git', 'apply', str(hooks)], cwd=args.output, check=True)
    for source in [ROOT / 'integration/ldb/host_capture.h', ROOT / 'integration/ldb/camera_state.h', ROOT / 'tools/dxvk_capture_shared.h', ROOT / 'tools/gpu_reprojection.h', ROOT / 'tools/camera_reproject_spirv.h']:
        shutil.copy2(source, server / source.name)
    generate_interop(args.dxvk, server / 'dxvk_interop.h')
    for name in ('vulkan', 'vk_video'):
        shutil.copytree(args.headers / 'include' / name, server / name)
    shutil.copytree(detours, args.output / 'submodules/Detours', ignore=shutil.ignore_patterns('.git'))
    meson = server / 'meson.build'
    text = meson.read_text()
    old = 'include_directories : [ bridge_include_path, util_include_path, public_include_path ],'
    if text.count(old) != 1:
        raise RuntimeError('Unexpected Host Meson includes')
    meson.write_text(text.replace(old, old.replace('public_include_path', "public_include_path, include_directories('.')")))
    receipt = {**baseline, 'hooks_sha256': digest(hooks), 'host_capture_sha256': digest(server / 'host_capture.h'),
               'shared_capture_sha256': digest(server / 'dxvk_capture_shared.h'),
               'gpu_sources_sha256': {name:digest(server/name) for name in ['camera_state.h','gpu_reprojection.h','camera_reproject_spirv.h']},
               'native_host_build': 'unrun', 'l4d2_game_capture': 'unrun'}
    receipt['build_fingerprint']=hashlib.sha256(json.dumps(receipt,sort_keys=True).encode()).hexdigest()
    (args.output / 'capture-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    # Meson requires a local HEAD for its generated version resource.
    subprocess.run(['git', 'add', '.'], cwd=args.output, check=True)
    subprocess.run(['git', '-c', 'user.name=Codex', '-c', 'user.email=codex@local.invalid',
                    'commit', '-q', '-m', 'Pinned LDB Host capture source fixture'], cwd=args.output, check=True)
    print(args.output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bridge', type=Path, default=ROOT / '.research/sources/bridge-131')
    p.add_argument('--upstream', type=Path, default=ROOT / '.research/sources/bridge-upstream')
    p.add_argument('--dxvk', type=Path, default=ROOT / '.research/sources/dxvk')
    p.add_argument('--headers', type=Path, default=ROOT / '.research/sources/vulkan_headers')
    p.add_argument('--output', type=Path, default=ROOT / '.research/ldb-capture-source')
    a = p.parse_args()
    for key in ('bridge', 'upstream', 'dxvk', 'headers', 'output'):
        setattr(a, key, getattr(a, key).resolve())
    try:
        prepare(a)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        p.exit(1, f'{e}\n')
