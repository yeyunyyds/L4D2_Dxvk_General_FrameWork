"""Prepare a new checkout of the exact L4D2 custom runtime and its submodules."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def prepare(output):
    if output.exists():
        raise RuntimeError('Preserve existing runtime output; choose a new --output')
    baseline = json.loads((ROOT / 'integration/rt/baseline.json').read_text())
    output.mkdir(parents=True)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(output), *args])
    git('init', '-q')
    git('remote', 'add', 'origin', baseline['runtime_repository'])
    git('fetch', '--depth=1', 'origin', baseline['runtime_commit'])
    git('checkout', '-q', '--detach', baseline['runtime_commit'])
    if git('rev-parse', 'HEAD').decode().strip() != baseline['runtime_commit']:
        raise RuntimeError('Runtime checkout differs from pin')
    api = output / 'public/include/remix/remix_c.h'
    receipt = {'schema': 1, 'repository': baseline['runtime_repository'],
               'commit': baseline['runtime_commit'], 'branch_reference': baseline['runtime_branch_reference'],
               'api_sha256': hashlib.sha256(api.read_bytes()).hexdigest(),
               'smoke_source_sha256': hashlib.sha256((ROOT / 'integration/rt/rt_runtime_smoke.cpp').read_bytes()).hexdigest(),
               'submodules': 'pending', 'runtime_execution': 'unrun', 'b580_rt': 'unrun'}
    def save():
        (output / 'rt-runtime-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    save()
    git('submodule', 'update', '--init', '--recursive', '--depth=1')
    receipt['submodules'] = 'ready'
    receipt['submodule_status'] = git('submodule', 'status', '--recursive').decode().splitlines()
    save()
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.research/rt-remix-runtime')
    args = parser.parse_args()
    try:
        prepare(args.output.resolve())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
