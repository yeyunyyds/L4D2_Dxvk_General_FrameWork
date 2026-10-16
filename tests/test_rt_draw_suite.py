from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import rt_draw_suite as suite
from package_rt_draw import observer_files, sha
import test_rt_draw as draw_cases


class PackageAndCompletionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        entries=observer_files()
        for name,data in entries.items():
            target=self.root/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        manifest={'files':{name:{'sha256':sha(data)} for name,data in entries.items()}}
        suite.save(self.root/'draw-observer-manifest.json',manifest)

    def test_packaged_entry_validates_configuration_and_probe_reads_its_own_layout(self):
        entry=self.root/'scripts/rt_draw_suite.py'
        process=subprocess.run([sys.executable,'-I',str(entry),'--verify-only'],capture_output=True,text=True)
        self.assertEqual(process.returncode,0,process.stderr)
        code='''import sys,json
from pathlib import Path
from unittest.mock import patch
root=Path(sys.argv[1]);sys.path.insert(0,str(root/'scripts'))
from rt_game_probe import collect
with patch('rt_game_probe.WindowsProcess',side_effect=OSError('fixture process unavailable')):
    result=collect(1,json.loads((root/'research/rt-source-audit.json').read_text()))
assert result['reference_mod_commit']==json.loads((root/'integration/rt/baseline.json').read_text())['compat_commit']
assert result['error']=='fixture process unavailable'
assert result['execution']=='incomplete'
'''
        process=subprocess.run([sys.executable,'-I','-c',code,str(self.root)],capture_output=True,text=True)
        self.assertEqual(process.returncode,0,process.stderr)

    def test_manifest_omission_is_rejected_even_if_other_hashes_pass(self):
        manifest=json.loads((self.root/'draw-observer-manifest.json').read_text())
        manifest['files'].pop('integration/rt/baseline.json')
        suite.save(self.root/'draw-observer-manifest.json',manifest)
        with self.assertRaisesRegex(ValueError,'configuration missing'):
            suite.verify_package(self.root)

    def test_configuration_tampering_is_rejected_before_install(self):
        (self.root/'integration/rt/baseline.json').write_text('{}')
        with self.assertRaisesRegex(ValueError,'checksum mismatch'):
            suite.verify_package(self.root)

    def test_world_catalog_is_required_and_verified_before_install_or_download(self):
        path=self.root/'draw-observer-manifest.json';manifest=json.loads(path.read_text());manifest['profile']='world_pilot'
        suite.save(path,manifest)
        with self.assertRaisesRegex(ValueError,'catalog missing'):suite.verify_package(self.root)
        name='integration/rt/world-runtime.json'
        data=(Path(suite.__file__).resolve().parents[1]/name).read_bytes()
        (self.root/name).write_bytes(data);manifest['files'][name]={'sha256':sha(data)};suite.save(path,manifest)
        self.assertEqual(suite.verify_package(self.root)['profile'],'world_pilot')

    def trace(self):
        directory=self.root/'results';directory.mkdir()
        events=draw_cases.DrawObservation().fixture()
        import analyze_rt_draw as check
        data=b''.join(check.RECORD.pack(e,32+len(p),u,19,0,i+1)+p for i,(e,u,p) in enumerate(events))
        (directory/'rt-draw-host-fixture.bin').write_bytes(data)
        return directory

    def test_module_failure_keeps_completed_draw_analysis_and_partial_scope(self):
        directory=self.trace();report={'errors':[]}
        with patch.object(suite,'collect',side_effect=FileNotFoundError('fixture missing baseline')):
            suite.finish_capture(directory,self.root,123,report,lambda message:None)
        self.assertEqual(report['draw_observation'],'pass')
        self.assertEqual(report['module_evidence'],'incomplete')
        self.assertEqual(report['collection'],'incomplete')
        self.assertTrue(json.loads((directory/'draw-analysis.json').read_text())['rt-draw-host-fixture.bin']['pass'])
        self.assertIn('FileNotFoundError',json.loads((directory/'game-binding.json').read_text())['error'])

    def test_invalid_trace_does_not_block_independent_module_evidence(self):
        directory=self.trace();report={'errors':[]}
        (directory/'rt-draw-host-fixture.bin').write_bytes(b'truncated')
        with patch.object(suite,'collect',return_value={'execution':'snapshot_collected'}):
            suite.finish_capture(directory,self.root,123,report,lambda message:None)
        self.assertEqual(report['draw_observation'],'fail_or_partial')
        self.assertEqual(report['module_evidence'],'snapshot_collected')
        self.assertEqual(report['collection'],'incomplete')

    def test_startup_inventory_survives_missing_f9_and_reports_inaccessible_host(self):
        directory=self.root/'startup';directory.mkdir();messages=[]
        def processes(diagnostics):
            diagnostics['related_processes']=[{'pid':456,'exe':'L4D2Bridge64.exe'}]
            return [123]
        with patch.object(suite,'game_pids',side_effect=processes),patch.object(suite,'WindowsProcess') as process:
            context=process.return_value.__enter__.return_value
            context.identity.return_value={'alive':True}
            context.modules.side_effect=[[{'name':'engine.dll'}],OSError('host exited')]
            suite.startup_modules(directory,123,messages.append)
        saved=json.loads((directory/'startup-modules.json').read_text())
        self.assertEqual(saved['processes'][0]['modules'],[{'name':'engine.dll'}])
        self.assertEqual(saved['processes'][1]['error'],'host exited')
        self.assertIn('does not mean',messages[0])
        self.assertFalse(list(directory.glob('rt-draw-done-*')))


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

    def test_new_package_inherits_original_backup_instead_of_backing_up_the_observer(self):
        suite.install(self.root,self.game,self.manifest)
        other=self.root.parent/'new-world-package';other.mkdir()
        manifest={'binaries':{}}
        for i,name in enumerate(suite.TARGETS):
            path=other/'payload'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'world'+bytes([i]))
            manifest['binaries'][name]={'sha256':suite.digest(path)}
        receipt=suite.install(other,self.game,manifest)
        for i,name in enumerate(suite.TARGETS):self.assertEqual(Path(receipt['files'][name]['backup']).read_bytes(),b'old'+bytes([i]))
        suite.restore(other)
        for i,name in enumerate(suite.TARGETS):self.assertEqual((self.game/name).read_bytes(),b'old'+bytes([i]))

    def test_failed_upgrade_rolls_back_previous_observer_and_can_restore_original(self):
        suite.install(self.root,self.game,self.manifest)
        other=self.root.parent/'failed-world-package';other.mkdir();manifest={'binaries':{}}
        for i,name in enumerate(suite.TARGETS):
            path=other/'payload'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'world'+bytes([i]))
            manifest['binaries'][name]={'sha256':suite.digest(path)}
        original=suite.replace_copy;calls=[]
        def locked(source,target):
            calls.append(target)
            if len(calls)==2:raise PermissionError('fixture locked second upgrade file')
            return original(source,target)
        with patch.object(suite,'replace_copy',side_effect=locked):
            with self.assertRaises(PermissionError):suite.install(other,self.game,manifest)
        for i,name in enumerate(suite.TARGETS):self.assertEqual((self.game/name).read_bytes(),b'new'+bytes([i]))
        suite.restore(other)
        for i,name in enumerate(suite.TARGETS):self.assertEqual((self.game/name).read_bytes(),b'old'+bytes([i]))

    def test_isolated_launcher_can_import_its_shipped_helpers(self):
        p=subprocess.run([sys.executable,'-I',str(Path(suite.__file__)),'--help'],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr)


if __name__=='__main__':unittest.main()
