"""Prepare a separate pinned RT observer Bridge; never includes FG assets."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(path, *arguments):
    return subprocess.check_output(['git', '-C', str(path), *arguments])


def source_object(path, repository, commit):
    """Read pinned git objects without checking out or overwriting local changes."""
    if not path.exists():
        path.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(path)], check=True)
        subprocess.run(['git', '-C', str(path), 'remote', 'add', 'origin', repository], check=True)
    top = Path(git(path, 'rev-parse', '--show-toplevel').decode().strip()).resolve()
    if top != path.resolve():
        raise RuntimeError(f'Not an independent source cache: {path}')
    found = subprocess.run(['git', '-C', str(path), 'cat-file', '-e', commit + '^{commit}'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if found.returncode:
        subprocess.run(['git', '-C', str(path), 'fetch', '--depth=1', 'origin', commit], check=True)
    return path


def prepare(output, bridge, upstream, detours):
    if output.exists():
        raise RuntimeError('Preserve existing output; select a new --output directory')
    baseline = json.loads((ROOT / 'integration/rt/baseline.json').read_text())
    for path, prefix in [(bridge, 'bridge'), (upstream, 'upstream'), (detours, 'detours')]:
        source_object(path, baseline[prefix + '_repository'], baseline[prefix + '_commit'])
    patch = git(bridge, 'show', baseline['bridge_commit'] + ':patches/l4d2-bridge.patch')
    if hashlib.sha256(patch).hexdigest() != baseline['bridge_patch_sha256']:
        raise RuntimeError('RT Bridge baseline patch hash differs')
    archive = git(upstream, 'archive', baseline['upstream_commit'], 'bridge', 'public', 'LICENSE',
                  'src/util/util_per_game_config.h', 'src/util/util_crash_report.h', 'src/util/xxHash')
    output.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(output, filter='data')
    subprocess.run(['git', 'init', '-q', str(output)], check=True)
    base_patch = output / 'ldb-baseline.patch'
    base_patch.write_bytes(patch)
    hooks = ROOT / 'integration/rt/bridge-hooks.patch'
    for current in [base_patch, hooks]:
        subprocess.run(['git', '-C', str(output), 'apply', '--check', str(current)], check=True)
        subprocess.run(['git', '-C', str(output), 'apply', str(current)], check=True)
    base_patch.unlink()
    inputs = {'bridge-hooks.patch': digest(hooks)}
    for role, names in [('client', ['rt_protocol.h', 'rt_bridge_api.h', 'rt_client.h']),
                        ('server', ['rt_protocol.h', 'rt_host.h'])]:
        for name in names:
            source = ROOT / 'integration/rt' / name
            shutil.copy2(source, output / 'bridge/src' / role / name)
            inputs[name] = digest(source)
    detours_archive = git(detours, 'archive', baseline['detours_commit'])
    destination = output / 'submodules/Detours'
    destination.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(detours_archive)) as tar:
        tar.extractall(destination, filter='data')
    receipt = {**baseline, 'inputs_sha256': inputs,
               'upstream_archive_sha256': hashlib.sha256(archive).hexdigest(),
               'native_build': 'unrun', 'game_queue_execution': 'unrun',
               'contains_fg_assets': False, 'runtime_dll_included': False}
    receipt['fingerprint'] = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
    (output / 'rt-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    subprocess.run(['git', '-C', str(output), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(output), '-c', 'user.name=Codex', '-c', 'user.email=codex@local.invalid',
                    'commit', '-qm', 'Pinned LDB RT observer source'], check=True)
    print(output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT / '.research/rt-observer-source')
    p.add_argument('--bridge', type=Path, default=ROOT / '.research/sources/bridge-131')
    p.add_argument('--upstream', type=Path, default=ROOT / '.research/sources/bridge-upstream')
    p.add_argument('--detours', type=Path, default=ROOT / '.research/sources/detours')
    a = p.parse_args()
    try:
        prepare(*(getattr(a, key).resolve() for key in ['output', 'bridge', 'upstream', 'detours']))
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        p.exit(1, f'{e}\n')
