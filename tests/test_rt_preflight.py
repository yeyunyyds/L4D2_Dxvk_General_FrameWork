"""Exercise dependency integrity, process timeouts and retained one-ZIP results."""

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import rt_preflight as runner
from package_rt_preflight import payloads


class PreflightCases(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / 'bundles'; self.cache.mkdir()

    def fixture(self, members=None):
        members = members or {'d3d9.dll': b'fixed runtime', 'rt-runtime-smoke.exe': b'diagnostic'}
        sources = self.root / 'inputs'; sources.mkdir()
        files = {}
        for name, data in members.items():
            path = sources / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            files[name] = path
        parts = payloads(files, self.cache)
        manifest = {'schema': 1, 'scope': 'rt_prerequisites', 'build_commit': 'test',
                    'runtime_files': {name: {'sha256': runner.digest(path), 'size': path.stat().st_size}
                                      for name, path in files.items()}, 'payloads': parts}
        (self.root / 'preflight-manifest.json').write_text(json.dumps(manifest))
        (self.root / 'research').mkdir(exist_ok=True)
        (self.root / 'research/rt-source-audit.json').write_text('{}')
        return manifest

    def test_complete_cache_needs_no_network_and_tampered_file_is_repaired(self):
        manifest = self.fixture()
        with patch('urllib.request.urlopen', side_effect=AssertionError('Unexpected network')):
            runtime = runner.ensure_runtime(self.root, manifest, self.cache)
            (runtime / 'd3d9.dll').write_bytes(b'corrupt')
            self.assertEqual(runner.ensure_runtime(self.root, manifest, self.cache), runtime)
            self.assertEqual((runtime / 'd3d9.dll').read_bytes(), b'fixed runtime')
            self.assertEqual(runner.ensure_runtime(self.root, manifest, self.cache), runtime)

    def test_wrong_download_hash_is_never_extracted(self):
        manifest = self.fixture()
        manifest['payloads'][0]['sha256'] = '0' * 64
        path = self.cache / manifest['payloads'][0]['name']
        with patch('urllib.request.urlopen', side_effect=lambda *_a, **_k: path.open('rb')):
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                runner.ensure_runtime(self.root, manifest, self.cache)
        self.assertFalse((self.root / 'cache/runtime/d3d9.dll').exists())
        self.assertFalse(list(self.cache.glob('*.download')))

    def test_unknown_or_traversing_member_is_rejected_before_write(self):
        manifest = self.fixture()
        path = self.cache / manifest['payloads'][0]['name']
        with zipfile.ZipFile(path, 'a') as archive:
            archive.writestr('../escaped.dll', b'unsafe')
        manifest['payloads'][0].update(sha256=runner.digest(path), size=path.stat().st_size)
        with self.assertRaisesRegex(ValueError, 'Unsafe package member'):
            runner.ensure_runtime(self.root, manifest, self.cache)
        self.assertFalse((self.root / 'cache/escaped.dll').exists())

    def test_duplicate_member_does_not_silently_override_dependency(self):
        manifest = self.fixture()
        manifest['payloads'].append(manifest['payloads'][0])
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            runner.ensure_runtime(self.root, manifest, self.cache)

    def test_invalid_sizes_and_incomplete_catalog_are_rejected(self):
        manifest = self.fixture()
        manifest['runtime_files']['d3d9.dll']['size'] = runner.MAX_UNPACKED
        with self.assertRaisesRegex(ValueError, 'catalog'):
            runner.ensure_runtime(self.root, manifest, self.cache)
        manifest['runtime_files']['d3d9.dll']['size'] = 1
        with self.assertRaisesRegex(ValueError, 'member'):
            runner.ensure_runtime(self.root, manifest, self.cache)

    def test_split_payloads_are_independent_valid_zip_files_under_budget(self):
        files = {}
        for i in range(4):
            path = self.root / str(i); path.write_bytes(os.urandom(10000)); files[path.name] = path
        with patch('package_rt_preflight.MAX_PART', 20000):
            parts = payloads(files, self.cache)
        self.assertEqual(len(parts), 4)
        for part in parts:
            path = self.cache / part['name']
            self.assertLess(path.stat().st_size, 20000)
            with zipfile.ZipFile(path) as archive:
                self.assertIsNone(archive.testzip())

    def test_owned_child_timeout_preserves_partial_logs_and_reaps_child(self):
        stage = self.root / 'stage'
        result = runner.run_owned([sys.executable, '-c', 'import time; print("started", flush=True); time.sleep(30)'],
                                  stage, timeout=0.3)
        self.assertEqual(result['execution'], 'fail')
        self.assertEqual(result['reason'], 'timeout')
        self.assertIn('started', (stage / 'stdout.log').read_text())
        self.assertIsNotNone(result['exit_code'])

    def test_owned_child_output_budget_stops_verbose_process(self):
        result = runner.run_owned([sys.executable, '-c', 'import time; print("x"*10000, flush=True); time.sleep(30)'],
                                  self.root / 'stage', byte_limit=1000)
        self.assertEqual(result['reason'], 'output_budget')

    def test_runtime_failure_keeps_independent_game_evidence_in_one_zip(self):
        self.fixture()
        with patch.object(runner, 'ensure_runtime', side_effect=OSError('dependency unavailable')), \
                patch.object(runner, 'collect', return_value={'execution': 'snapshot_collected'}):
            report, archive = runner.run(self.root, pid=123)
        self.assertEqual(report['runtime_preparation'], 'fail')
        self.assertEqual(report['game_binding'], 'snapshot_collected')
        self.assertEqual(report['game_rt'], 'unrun')
        self.assertEqual(report['source_threading'], 'unrun')
        self.assertEqual(len(list((self.root / 'results').glob('*.zip'))), 1)
        with zipfile.ZipFile(archive) as result:
            self.assertIn('game-binding.json', result.namelist())
            self.assertEqual(json.loads(result.read('summary.json'))['errors'][0]['stage'], 'runtime')

    def test_oversized_result_is_reported_as_incomplete(self):
        directory = self.root / 'result'; directory.mkdir()
        (directory / 'large.log').write_bytes(b'x' * 1000)
        (directory / 'small.json').write_text('{}')
        report = {'collection': 'complete'}
        archive = runner.pack_results(directory, report, limit=50)
        self.assertEqual(report['collection'], 'incomplete')
        self.assertEqual(report['omitted_files'][0]['path'], 'large.log')
        with zipfile.ZipFile(archive) as result:
            self.assertIn('small.json', result.namelist())
            self.assertNotIn('large.log', result.namelist())


if __name__ == '__main__':
    unittest.main()
