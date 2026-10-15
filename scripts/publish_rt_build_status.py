"""Publish RT ZIPs atomically to the download branch without a worktree."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def git(*args, **kwargs):
    return subprocess.check_output(['git', *args], **kwargs).decode().strip()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--package', type=Path)
    group.add_argument('--packages-directory', type=Path)
    args = parser.parse_args()
    if args.packages_directory:
        import json
        index = json.loads((args.packages_directory / 'publish-index.json').read_text())
        if index['package_validation'] != 'pass' or index['commit'] != os.environ['GITHUB_SHA']:
            raise SystemExit('Only the current, verified prerequisite bundle may be published')
        packages = [args.packages_directory / index['launcher']] + [
            args.packages_directory / part['name'] for part in index['payloads']]
    else:
        packages = [args.package or Path('.research/dist/rt-foundation-build-status.zip')]
    for package in packages:
        if package.suffix.lower() != '.zip':
            raise SystemExit('Only ZIP files can be published to the download branch')
        if not package.is_file() or package.stat().st_size > 95 * 1024 * 1024:
            raise SystemExit('ZIP missing or exceeds Git size budget')
    subprocess.run(['git', 'config', 'user.name', 'github-actions[bot]'], check=True)
    subprocess.run(['git', 'config', 'user.email', '41898282+github-actions[bot]@users.noreply.github.com'], check=True)
    for attempt in range(3):
        subprocess.run(['git', 'fetch', 'origin', 'zip-host-capture'], check=True)
        parent = git('rev-parse', 'FETCH_HEAD')
        with tempfile.TemporaryDirectory(prefix='rt-index-') as directory:
            environment = {**os.environ, 'GIT_INDEX_FILE': str(Path(directory) / 'index')}
            subprocess.run(['git', 'read-tree', parent], env=environment, check=True)
            for package in packages:
                blob = git('hash-object', '-w', str(package))
                subprocess.run(['git', 'update-index', '--add', '--cacheinfo',
                                '100644,' + blob + ',downloads/' + package.name], env=environment, check=True)
            tree = git('write-tree', env=environment)
            commit = git('commit-tree', tree, '-p', parent,
                         input=('Publish RT foundation build status ' + os.environ['GITHUB_SHA'] + '\n').encode())
        result = subprocess.run(['git', 'push', 'origin', commit + ':refs/heads/zip-host-capture'])
        if not result.returncode:
            print(commit)
            break
    else:
        raise SystemExit('Download branch changed repeatedly; preserve package as CI artifact')
