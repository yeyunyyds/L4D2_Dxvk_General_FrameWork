from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import rt_draw_suite as suite


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)/'package';self.root.mkdir()
        self.game=Path(self.temp.name)/'game';self.game.mkdir()
        self.manifest={'binaries':{}}
        for i,name in enumerate(suite.TARGETS):
            previous=self.game/name;previous.parent.mkdir(parents=True,exist_ok=True);previous.write_bytes(b'old'+bytes([i]))
            new=self.root/'payload'/name;new.parent.mkdir(parents=True,exist_ok=True);new.write_bytes(b'new'+bytes([i]))
            self.manifest['binaries'][name]={'sha256':suite.digest(new)}

    def test_install_restores_exact_original_and_preserves_backups(self):
        receipt=suite.install(self.root,self.game,self.manifest)
        self.assertEqual(suite.install(self.root,self.game,self.manifest),receipt)
        for name in suite.TARGETS:self.assertEqual(suite.digest(self.game/name),receipt['files'][name]['installed_sha256'])
        suite.restore(self.root);suite.restore(self.root)
        for i,name in enumerate(suite.TARGETS):self.assertEqual((self.game/name).read_bytes(),b'old'+bytes([i]))

    def test_failed_second_replace_rolls_back_first_file(self):
        original=suite.replace_copy;calls=[]
        def replace(src,dst):
            calls.append(str(dst))
            if len(calls)==2:raise PermissionError('fixture locked Host')
            return original(src,dst)
        with patch.object(suite,'replace_copy',side_effect=replace):
            with self.assertRaises(PermissionError):suite.install(self.root,self.game,self.manifest)
        for i,name in enumerate(suite.TARGETS):self.assertEqual((self.game/name).read_bytes(),b'old'+bytes([i]))

    def test_restore_refuses_to_overwrite_subsequent_user_changes(self):
        suite.install(self.root,self.game,self.manifest)
        (self.game/suite.TARGETS[0]).write_bytes(b'user update')
        with self.assertRaisesRegex(ValueError,'changed'):suite.restore(self.root)
        self.assertEqual((self.game/suite.TARGETS[0]).read_bytes(),b'user update')

    def test_isolated_launcher_can_import_its_shipped_helpers(self):
        p=subprocess.run([sys.executable,'-I',str(Path(suite.__file__)),'--help'],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr)


if __name__=='__main__':unittest.main()
