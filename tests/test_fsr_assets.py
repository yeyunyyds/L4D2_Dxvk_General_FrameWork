"""Reject stale source and altered prebuilt modules before native packaging."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import fetch_fsr_assets

class FsrAssetTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name);(self.root/'tools').mkdir();(self.root/'research').mkdir()
        self.source=self.root/'tools/implementation.cpp';self.source.write_text('compiled source\n')
        self.payload=b'fixture module bytes'
        self.revision='a'*40
        self.cache=self.root/'.research/fsr-assets'/f'{self.revision}.zip'
        self.cache.parent.mkdir(parents=True)
        self.cache.write_bytes(self.archive({'bin/.l4d2bridge/LDBFsrBackend64.dll':self.payload}))
        self.lock={'download_commit':self.revision,'archive_sha256':self.digest(self.cache.read_bytes()),
                   'files':{'bin/.l4d2bridge/LDBFsrBackend64.dll':self.digest(self.payload)},
                   'project_source_hashes':{'tools/implementation.cpp':self.digest(self.source.read_bytes())}}
        self.save()
        self.root_patch=patch.object(fetch_fsr_assets,'ROOT',self.root);self.root_patch.start();self.addCleanup(self.root_patch.stop)
        self.network=patch.object(fetch_fsr_assets.subprocess,'run',side_effect=AssertionError('Unexpected fetch'))
        self.network.start();self.addCleanup(self.network.stop)
    @staticmethod
    def digest(data):return hashlib.sha256(data).hexdigest()
    @staticmethod
    def archive(files):
        stream=io.BytesIO()
        with zipfile.ZipFile(stream,'w') as z:
            for name,data in files.items():z.writestr(name,data)
        return stream.getvalue()
    def save(self):(self.root/'research/fsr-backend-assets.json').write_text(json.dumps(self.lock))
    def test_matching_cached_modules(self):
        _,files=fetch_fsr_assets.obtain_assets()
        self.assertEqual(files['bin/.l4d2bridge/LDBFsrBackend64.dll'],self.payload)
    def test_source_changed_after_compile(self):
        self.source.write_text('newer uncompiled source\n')
        with self.assertRaisesRegex(RuntimeError,'does not match project source'):fetch_fsr_assets.obtain_assets()
        self.assertEqual(self.source.read_text(),'newer uncompiled source\n')
    def test_corrupted_cache_preserved_and_rejected(self):
        self.cache.write_bytes(b'corrupt')
        with self.assertRaisesRegex(RuntimeError,'ZIP hash mismatch'):fetch_fsr_assets.obtain_assets()
        self.assertEqual(self.cache.read_bytes(),b'corrupt')
    def test_extra_unregistered_file_rejected(self):
        self.cache.write_bytes(self.archive({'bin/.l4d2bridge/LDBFsrBackend64.dll':self.payload,'unexpected.exe':b'extra'}))
        self.lock['archive_sha256']=self.digest(self.cache.read_bytes());self.save()
        with self.assertRaisesRegex(RuntimeError,'contents mismatch'):fetch_fsr_assets.obtain_assets()

if __name__=='__main__':unittest.main()
