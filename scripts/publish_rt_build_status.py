"""Add only the RT evidence ZIP to the download branch without a worktree."""

import os
from pathlib import Path
import subprocess
import tempfile


def git(*args, **kwargs):
    return subprocess.check_output(['git', *args], **kwargs).decode().strip()


if __name__ == '__main__':
    package = Path('.research/dist/rt-foundation-build-status.zip')
    if not package.is_file():
        raise SystemExit('Missing status ZIP')
    subprocess.run(['git', 'config', 'user.name', 'github-actions[bot]'], check=True)
    subprocess.run(['git', 'config', 'user.email', '41898282+github-actions[bot]@users.noreply.github.com'], check=True)
    for attempt in range(3):
        subprocess.run(['git', 'fetch', 'origin', 'zip-host-capture'], check=True)
        parent = git('rev-parse', 'FETCH_HEAD')
        with tempfile.TemporaryDirectory(prefix='rt-index-') as directory:
            environment = {**os.environ, 'GIT_INDEX_FILE': str(Path(directory) / 'index')}
            subprocess.run(['git', 'read-tree', parent], env=environment, check=True)
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
