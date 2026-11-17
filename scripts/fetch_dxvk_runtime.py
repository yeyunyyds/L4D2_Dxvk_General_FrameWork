"""Fetch the hash-pinned, unmodified upstream DXVK x64 D3D9 runtime."""
import hashlib
import io
import json
import tarfile
import urllib.request

from prepare_research import ROOT


def obtain_dxvk_runtime():
    lock = json.loads((ROOT / 'research/dxvk-runtime-assets.json').read_text())
    cache = ROOT / '.research/dxvk-runtime' / (
        lock['version'] + '-' + lock['archive_sha256'][:16] + '.tar.gz')
    if cache.exists():
        archive = cache.read_bytes()
    else:
        with urllib.request.urlopen(lock['url'], timeout=60) as response:
            archive = response.read(32 * 1024 * 1024 + 1)
        if len(archive) > 32 * 1024 * 1024:
            raise RuntimeError('Unexpected DXVK archive size')
    if hashlib.sha256(archive).hexdigest() != lock['archive_sha256']:
        raise RuntimeError('Official DXVK archive hash mismatch')
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as package:
        member = package.getmember(lock['member'])
        if not member.isfile() or member.size > 16 * 1024 * 1024:
            raise RuntimeError('Unexpected DXVK D3D9 member')
        payload = package.extractfile(member).read()
    if hashlib.sha256(payload).hexdigest() != lock['member_sha256']:
        raise RuntimeError('Official DXVK D3D9 hash mismatch')
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        cache.write_bytes(archive)
    return lock, payload


if __name__ == '__main__':
    metadata, _ = obtain_dxvk_runtime()
    print('Verified official DXVK runtime:', metadata['version'])
