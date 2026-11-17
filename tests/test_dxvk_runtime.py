"""Verify the upstream DXVK runtime lock without using the network."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import fetch_dxvk_runtime


class DxvkRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'research').mkdir()
        self.payload = b'MZ official upstream fixture'
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as archive:
            info = tarfile.TarInfo('dxvk/x64/d3d9.dll')
            info.size = len(self.payload)
            archive.addfile(info, io.BytesIO(self.payload))
        self.archive = stream.getvalue()
        self.lock = {
            'schema': 1,
            'version': 'fixture',
            'url': 'https://invalid.example/dxvk.tar.gz',
            'archive_sha256': self.digest(self.archive),
            'member': 'dxvk/x64/d3d9.dll',
            'member_sha256': self.digest(self.payload),
        }
        (self.root / 'research/dxvk-runtime-assets.json').write_text(
            json.dumps(self.lock))
        cache = self.root / '.research/dxvk-runtime' / (
            'fixture-' + self.lock['archive_sha256'][:16] + '.tar.gz')
        cache.parent.mkdir(parents=True)
        cache.write_bytes(self.archive)
        root_patch = patch.object(fetch_dxvk_runtime, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        network = patch.object(fetch_dxvk_runtime.urllib.request, 'urlopen',
                               side_effect=AssertionError('Unexpected network'))
        network.start()
        self.addCleanup(network.stop)

    @staticmethod
    def digest(data):
        return hashlib.sha256(data).hexdigest()

    def test_hash_pinned_member_is_returned(self):
        metadata, payload = fetch_dxvk_runtime.obtain_dxvk_runtime()
        self.assertEqual(metadata['version'], 'fixture')
        self.assertEqual(payload, self.payload)

    def test_corrupt_cached_archive_is_rejected(self):
        cache = next((self.root / '.research/dxvk-runtime').iterdir())
        cache.write_bytes(b'corrupt')
        with self.assertRaisesRegex(RuntimeError, 'archive hash mismatch'):
            fetch_dxvk_runtime.obtain_dxvk_runtime()


if __name__ == '__main__':
    unittest.main()
