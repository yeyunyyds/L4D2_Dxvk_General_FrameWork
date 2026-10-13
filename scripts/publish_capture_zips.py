"""Update an existing ZIP-only branch without force pushes or source files."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
from prepare_research import ROOT

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--worktree', type=Path, required=True)
a = p.parse_args()
worktree = a.worktree.resolve()
files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=worktree).decode().split('\0')
if any(name and not name.lower().endswith('.zip') for name in files):
    raise RuntimeError('Download branch is not ZIP-only; existing files preserved')
source = ROOT / '.research/dist'
status_zip = source / 'ldb-host-capture-build-status.zip'
with zipfile.ZipFile(status_zip) as z:
    if z.testzip() is not None: raise RuntimeError('Corrupt CI status ZIP')
    status = json.loads(z.read('status.json'))
if status.get('framework_commit') != subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip():
    raise RuntimeError('Status is from another source commit')
paths = [status_zip]
if status.get('native_build_outcome') == 'success':
    package = source / 'ldb-host-capture-dev.zip'
    with zipfile.ZipFile(package) as z:
        if z.testzip() is not None: raise RuntimeError('Corrupt native package')
        receipt = json.loads(z.read('capture-build.json'))
        if receipt.get('native_host_build') != 'compiled_MSVC_Release':
            raise RuntimeError('No verified native build receipt')
    import hashlib
    if hashlib.sha256(package.read_bytes()).hexdigest() != status.get('package_sha256'):
        raise RuntimeError('Package differs from CI status')
    paths.append(package)
else:
    # Remove a previous package to avoid offering stale binaries for this attempt.
    previous = worktree / 'downloads/ldb-host-capture-dev.zip'
    if previous.exists(): previous.unlink()
destination = worktree / 'downloads'
destination.mkdir(exist_ok=True)
for path in paths: shutil.copyfile(path, destination / path.name)
subprocess.run(['git', 'add', '--all', 'downloads'], cwd=worktree, check=True)
subprocess.run(['git', '-c', 'user.name=github-actions[bot]', '-c', 'user.email=41898282+github-actions[bot]@users.noreply.github.com',
                'commit', '-m', 'Publish LDB capture build ' + status['framework_commit'][:12]], cwd=worktree, check=True)
subprocess.run(['git', 'push', 'origin', 'HEAD:refs/heads/zip-host-capture'], cwd=worktree, check=True)
