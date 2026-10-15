"""Archive the pinned L4D2 SDK for native callback/ABI development checks."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

from prepare_rt_bridge import ROOT, git, source_object


def prepare(output, cache):
    if output.exists():
        raise RuntimeError('Preserve existing SDK output; choose another --output')
    reference = json.loads((ROOT / 'research/rt-source-queue-sdk.json').read_text())
    source_object(cache, reference['repository'], reference['commit'])
    data = git(cache, 'archive', reference['commit'], 'public', 'common')
    output.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(output, filter='data')
    for item in reference['sources']:
        path = output / item['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError('Pinned SDK file hash differs: ' + item['path'])
    receipt = {'schema': 1, 'repository': reference['repository'], 'commit': reference['commit'],
               'archive_sha256': hashlib.sha256(data).hexdigest(),
               'game_abi_verified': False, 'game_hook_installed': False,
               'note': 'Unmodified SDK headers retain their original copyright notices.'}
    (output / 'rt-sdk-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.research/rt-sdk-source')
    parser.add_argument('--cache', type=Path, default=ROOT / '.research/sources/sdk-l4d2')
    args = parser.parse_args()
    try:
        prepare(args.output.resolve(), args.cache.resolve())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
