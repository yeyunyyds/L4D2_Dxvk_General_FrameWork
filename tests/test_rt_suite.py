"""Execute process orchestration, failure retention and one-ZIP packaging."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from rt_suite import run, sha256


class SuiteCases(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.helper = self.root / 'child.py'
        self.helper.write_text('''import os, pathlib, sys, time
mode = sys.argv[1]
assert os.environ['LDB_FG_ENABLE'] == '0'
assert os.environ['LDB_FG_HOTKEYS'] == '0'
pathlib.Path('observed.txt').write_text(mode)
if mode == 'timeout': time.sleep(10)
if mode == 'budget':
    print('x' * 10000, flush=True)
    time.sleep(10)
if mode == 'fail': raise SystemExit(7)
''')

    def stage(self, name, mode='good', depends=()):
        return {'id': name, 'argv': [sys.executable, str(self.helper), mode],
                'executable_sha256': sha256(sys.executable),
                'inputs': [{'path': str(self.helper), 'sha256': sha256(self.helper)}],
                'depends_on': list(depends), 'timeout_seconds': 3}

    def execute(self, stages, **options):
        manifest = {'schema': 1, 'scope': 'developer_foundation', 'stages': stages, **options}
        return run(manifest, self.root / 'session')

    def test_quality_and_performance_remain_separate_in_one_archive(self):
        report, archive = self.execute([self.stage('quality'), self.stage('performance', depends=['quality'])])
        self.assertEqual(report['execution'], 'pass')
        self.assertEqual(report['performance_acceptance'], 'unrun')
        self.assertEqual(report['source_game_queue'], 'unrun')
        self.assertEqual(len(list(self.root.glob('*.zip'))), 1)
        with zipfile.ZipFile(archive) as package:
            for name in ['quality', 'performance']:
                self.assertIn('runs/' + name + '/observed.txt', package.namelist())
            self.assertEqual(json.loads(package.read('summary.json'))['execution'], 'pass')

    def test_failure_skips_dependents_and_retains_independent_control(self):
        report, archive = self.execute([self.stage('bad', 'fail'), self.stage('dependent', depends=['bad']), self.stage('control')])
        self.assertEqual([x['execution'] for x in report['stages']], ['fail', 'unrun', 'pass'])
        self.assertTrue(archive.exists())
        self.assertEqual(report['stages'][0]['exit_code'], 7)

    def test_timeout_has_partial_archive(self):
        stage = self.stage('hung', 'timeout'); stage['timeout_seconds'] = 0.15
        report, archive = self.execute([stage])
        self.assertEqual(report['stages'][0]['reason'], 'timeout')
        self.assertEqual(report['execution'], 'fail')
        self.assertTrue(archive.exists())

    def test_log_budget_stops_only_owned_child(self):
        report, archive = self.execute([self.stage('verbose', 'budget')], max_log_bytes_per_stage=4096)
        self.assertEqual(report['stages'][0]['reason'], 'log_budget')
        self.assertTrue(archive.exists())

    def test_wrong_binary_never_launches_and_is_packaged(self):
        stage = self.stage('bad'); stage['executable_sha256'] = '0' * 64
        report, archive = self.execute([stage])
        self.assertIn('SHA256 mismatch', report['error'])
        self.assertEqual(report['stages'], [])
        self.assertTrue(archive.exists())

    def test_existing_session_is_preserved(self):
        directory = self.root / 'session'; directory.mkdir()
        marker = directory / 'user.txt'; marker.write_text('preserve')
        with self.assertRaises(FileExistsError):
            self.execute([self.stage('good')])
        self.assertEqual(marker.read_text(), 'preserve')


if __name__ == '__main__':
    unittest.main()
