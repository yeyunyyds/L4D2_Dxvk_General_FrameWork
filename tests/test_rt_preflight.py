"""Exercise dependency integrity, process timeouts and retained one-ZIP results."""

import json
import hashlib
import io
import os
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import rt_preflight as runner
import rt_visual as visual
from package_rt_preflight import payloads, pinned_vulkan_loader
from prepare_remix_runtime import (RUNTIME_SKINNING_PATCH, validate_runtime_skinning_source,
                                   validate_runtime_skinning_flush)


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

    def test_runtime_skinning_patch_corrects_weight_stride_and_rejects_bad_counts(self):
        patch_text = RUNTIME_SKINNING_PATCH.read_text()
        self.assertIn('+          dst.blendWeightBuffer = dxvk::RasterBuffer { blendWeightsSlice, 0, uint32_t(sizeof(float) * src.skinning_value.bonesPerVertex)', patch_text)
        self.assertIn('skinning.blendWeights_count != expectedSkinningValues', patch_text)
        self.assertIn('skinning.blendIndices_count != expectedSkinningValues', patch_text)
        self.assertIn('+        m_skinningCommands = 0;', patch_text)
        self.assertIn('kSkinningCommandsPerSubmission = 16', patch_text)
        corrected = b' '.join((
            b'expectedSkinningValues',
            b'skinning.blendWeights_count != expectedSkinningValues',
            b'skinning.blendIndices_count != expectedSkinningValues',
            b'sizeof(float) * src.skinning_value.bonesPerVertex',
        ))
        validate_runtime_skinning_source(corrected)
        with self.assertRaisesRegex(RuntimeError, 'one-float'):
            validate_runtime_skinning_source(corrected + b' blendWeightBuffer = dxvk::RasterBuffer { blendWeightsSlice, 0, sizeof(float),')
        geometry = b'm_skinningContext->flushCommandList();\n        m_skinningCommands = 0;'
        implementation = b'kSkinningCommandsPerSubmission = 16; m_skinningCommands >= kSkinningCommandsPerSubmission; m_skinningContext->flushCommandList();\n      m_skinningCommands = 0;'
        validate_runtime_skinning_flush(geometry, implementation)
        with self.assertRaisesRegex(RuntimeError, 'not reset'):
            validate_runtime_skinning_flush(b'm_skinningContext->flushCommandList();', implementation)
        with self.assertRaisesRegex(RuntimeError, 'not bounded'):
            validate_runtime_skinning_flush(geometry, b'')
        build_script = (Path(__file__).resolve().parents[1] / 'scripts/build_remix_runtime.ps1').read_text()
        self.assertIn('dbghelp.lib', build_script)
        packager = (Path(__file__).resolve().parents[1] / 'scripts/package_rt_preflight.py').read_text()
        for module in ('rt_preflight.py', 'rt_game_probe.py', 'rt_suite.py', 'rt_visual.py'):
            self.assertIn(repr(module), packager)

    def test_pinned_vulkan_loader_checks_outer_and_inner_hashes(self):
        payload = self.root / 'loader.zip'; output = self.root / 'vulkan-1.dll'; data = b'audited loader fixture'
        with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('vulkan-1.dll', data)
        digest = hashlib.sha256
        self.assertEqual(pinned_vulkan_loader(payload, output, digest(payload.read_bytes()).hexdigest(), digest(data).hexdigest()), output)
        self.assertEqual(output.read_bytes(), data)
        with self.assertRaisesRegex(ValueError, 'payload SHA256'):
            pinned_vulkan_loader(payload, output, '0' * 64, digest(data).hexdigest())

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

    def test_small_update_reuses_verified_runtime_files_without_unpacking_them(self):
        manifest = self.fixture()
        runtime = runner.ensure_runtime(self.root, manifest, self.cache)
        (runtime / 'rt-runtime-smoke.exe').write_bytes(b'corrupt')
        original = zipfile.ZipFile.open
        def open_member(archive, member, *args, **kwargs):
            if getattr(member, 'filename', member) == 'd3d9.dll':
                raise AssertionError('Verified large DLL must be reused')
            return original(archive, member, *args, **kwargs)
        with patch.object(zipfile.ZipFile, 'open', open_member):
            runner.ensure_runtime(self.root, manifest, self.cache)
        self.assertEqual((runtime / 'rt-runtime-smoke.exe').read_bytes(), b'diagnostic')

    def test_404_falls_back_to_github_and_keeps_pinned_revision_and_hash(self):
        manifest = self.fixture()
        part = manifest['payloads'][0]
        part['ref'] = 'a' * 40
        original = self.cache / part['name']
        data = original.read_bytes()
        original.unlink()
        requests, messages = [], []
        def get(request, **_):
            requests.append(request.full_url)
            if len(requests) == 1:
                raise urllib.error.HTTPError(request.full_url, 404, 'Not Found', {}, None)
            return io.BytesIO(data)
        with patch('urllib.request.urlopen', side_effect=get):
            runtime = runner.ensure_runtime(self.root, manifest, self.cache, notify=messages.append)
        self.assertEqual((runtime / 'd3d9.dll').read_bytes(), b'fixed runtime')
        self.assertEqual(len(requests), 2)
        self.assertTrue(all('/' + 'a' * 40 + '/' in url for url in requests))
        self.assertTrue(any('404' in text for text in messages))
        self.assertTrue(any('100%' in text for text in messages))

    def test_bundled_loader_payload_works_without_network(self):
        manifest = self.fixture()
        bundled = self.root / 'payloads'; bundled.mkdir()
        part = manifest['payloads'][0]
        shutil.move(self.cache / part['name'], bundled / part['name'])
        with patch('urllib.request.urlopen', side_effect=AssertionError('Unexpected network')):
            runtime = runner.ensure_runtime(self.root, manifest, self.cache)
        self.assertEqual((runtime / 'd3d9.dll').read_bytes(), b'fixed runtime')

    def test_game_without_renderer_is_collected_without_waiting_180_seconds(self):
        discovery, messages = {}, []
        with patch.object(runner, 'game_pids', return_value=[123]), \
                patch.object(runner, 'WindowsProcess') as process, \
                patch.object(runner.time, 'sleep', side_effect=AssertionError('Unexpected wait')):
            process.return_value.__enter__.return_value.modules.return_value = [
                {'name': 'client.dll'}, {'name': 'engine.dll'}, {'name': 'different-renderer.dll'}]
            self.assertEqual(runner.wait_game(diagnostics=discovery, notify=messages.append), 123)
        self.assertEqual(discovery['missing_core_modules'], [])
        self.assertIn({'name': 'different-renderer.dll'}, discovery['modules'])

    def test_browser_downloads_beside_launcher_work_without_network(self):
        manifest = self.fixture()
        part = manifest['payloads'][0]
        shutil.move(self.cache / part['name'], self.root / part['name'])
        with patch('urllib.request.urlopen', side_effect=AssertionError('Unexpected network')):
            runtime = runner.ensure_runtime(self.root, manifest, self.cache)
        self.assertEqual((runtime / 'd3d9.dll').read_bytes(), b'fixed runtime')

    def test_detection_failure_preserves_related_process_names_in_zip(self):
        self.fixture()
        def absent(**kwargs):
            kwargs['diagnostics']['related_processes'] = [{'exe': 'L4D2Bridge64.exe', 'pid': 50}]
            raise TimeoutError('No left4dead2.exe detected')
        with patch.object(runner, 'ensure_runtime', side_effect=OSError('HTTP Error 404')), \
                patch.object(runner, 'wait_game', side_effect=absent):
            report, archive = runner.run(self.root)
        self.assertEqual(report['game_binding'], 'fail')
        with zipfile.ZipFile(archive) as z:
            self.assertIn('L4D2Bridge64.exe', z.read('game-discovery.json').decode())
            progress = z.read('progress.log').decode()
            self.assertIn('FAILED [runtime]: OSError: HTTP Error 404', progress)
            self.assertIn('FAILED [game_binding]', progress)

    @unittest.skipUnless(os.name == 'nt' and os.environ.get('RT_GAME_PROBE_FIXTURE'),
                         'Requires native Windows x86 fixture')
    def test_native_process_discovery_finds_running_x86_game_name(self):
        executable = self.root / 'left4dead2.exe'
        shutil.copyfile(os.environ['RT_GAME_PROBE_FIXTURE'], executable)
        child = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            ready = json.loads(child.stdout.readline())
            discovery = {}
            self.assertIn(ready['pid'], runner.game_pids(discovery))
            self.assertIn({'pid': child.pid, 'exe': 'left4dead2.exe'}, discovery['related_processes'])
            self.assertEqual(runner.wait_game(timeout=0.01, diagnostics=discovery), child.pid)
            self.assertTrue(discovery['modules'])
            self.assertIsNone(child.poll())
        finally:
            child.communicate('q', timeout=5)
        self.assertEqual(child.returncode, 0)

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

    def test_rebuilt_diagnostic_integrity_is_independent_of_large_runtime_payloads(self):
        self.fixture()
        executable = self.root / 'bin/rt-runtime-smoke.exe'
        executable.parent.mkdir(); executable.write_bytes(b'new native diagnostic')
        manifest = {'diagnostic':{'path':'bin/rt-runtime-smoke.exe','sha256':runner.digest(executable),'size':executable.stat().st_size}}
        self.assertEqual(runner.smoke_program(self.root, manifest, self.root / 'cache/runtime'), executable)
        executable.write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'diagnostic SHA256'):
            runner.smoke_program(self.root, manifest, self.root / 'cache/runtime')

    @staticmethod
    def triangle(shift=0, color=(220, 90, 20)):
        # Independently rasterized screen triangle, derived from its documented
        # projected vertices (~112,284),(320,-28),(528,284), not checker masks.
        pixels = bytearray(bytes((2, 3, 4)) * (640 * 360))
        for y in range(284):
            half = (y + 28) * 2 / 3
            start = max(0, int(320 + shift - half) + 1)
            end = min(640, int(320 + shift + half))
            pixels[(y * 640 + start) * 3:(y * 640 + end) * 3] = bytes(color) * (end - start)
        return b'P6\n640 360\n255\n' + pixels

    def matrix_fixture(self):
        stage = self.root / 'runtime'; smoke = stage / 'smoke'; smoke.mkdir(parents=True)
        good = {'result':'static_visible_triangle_readback', 'cleanup':'complete', 'pick_hit':True,
                'image_has_signal':True, 'stable_image_signal':True, 'readback_successes':2}
        (smoke / 'status.json').write_text(json.dumps(good))
        for name in visual.ALL_CASES:
            case = smoke / name; case.mkdir()
            dark = name in visual.DARK_CASES
            status = good | ({'result':'static_expected_dark_readback'} if dark else {})
            (case / 'status.json').write_text(json.dumps(status))
            for part in ('early', 'final'):
                pixels = (b'P6\n640 360\n255\n' + bytes((2,3,4)) * (640*360)) if dark else self.triangle(104 if name in ('translated','light_translated') else 0)
                (case / (part + '-color.ppm')).write_bytes(pixels)
        (smoke / 'matrix.json').write_text(json.dumps({'schema':4,'selected_profile':'baseline','translated_offset_x':1.0}))
        events = [{'stage':'render_config','case':name,'option':option,'value':'False','code':0}
                  for name in visual.ALL_CASES for option in visual.SHARP_OPTIONS]
        (smoke / 'events.jsonl').write_text(''.join(json.dumps(e)+'\n' for e in events))
        (smoke / 'final-color.ppm').write_bytes(self.triangle())
        (smoke / 'geometry-input.json').write_text(json.dumps({'uvs':[[0,1],[0.5,0],[1,1]],'normals':[[0,0,-1]]*3}))
        (smoke / 'material-input.json').write_text(json.dumps({'materials':[
            {'variant':i,'alpha_test_type':7,'use_draw_call_alpha_state':False,'opacity':1,
             'blend_override':False,'emissive_intensity':intensity} for i,intensity in enumerate((4,64,0))]}))
        return stage, smoke, good

    def test_image_alone_or_hit_alone_or_crashed_cleanup_cannot_pass(self):
        stage, smoke, good = self.matrix_fixture()
        (stage / 'frame_trace.jsonl').write_text('{"camera_valid":true,"shader_compile_inflight_count":5}\n')
        # Root status is an additional requirement; the pixel checker is tested
        # separately below, keeping these status permutations fast and focused.
        with patch.object(runner, 'evaluate_matrix', return_value={'pass':True}):
            for changes in [{'pick_hit':False},{'stable_image_signal':False},{'cleanup':'pending'},{}]:
                with self.subTest(changes=changes):
                    (smoke / 'status.json').write_text(json.dumps(good | changes))
                    passed, evidence = runner.smoke_evidence(stage, {'execution':'pass'})
                    self.assertEqual(passed, not bool(changes))
                    self.assertEqual(evidence['readiness_trace'][0]['shader_compile_inflight_count'],5)
            self.assertFalse(runner.smoke_evidence(stage, {'execution':'fail'})[0])
            (smoke / 'crash.json').write_text('{}')
            self.assertFalse(runner.smoke_evidence(stage, {'execution':'pass'})[0])

    def test_projected_triangle_and_translation_are_checked_from_pixels(self):
        stage, smoke, good = self.matrix_fixture()
        passed, evidence = runner.smoke_evidence(stage, {'execution':'pass'})
        self.assertTrue(passed, evidence)
        self.assertTrue(evidence['visual_acceptance']['movement']['pass'])
        self.assertTrue((stage / 'visual-acceptance.json').is_file())
        # A correct hit/status does not make a stale unshifted image pass.
        (smoke / 'translated/final-color.ppm').write_bytes(self.triangle())
        self.assertFalse(visual.moved(smoke / 'baseline/final-color.ppm', smoke / 'translated/final-color.ppm')['pass'])
        self.assertFalse(visual.inspect(smoke / 'translated/final-color.ppm', 1.0)['pass'])

    def test_near_black_gray_hot_pixel_and_flat_orange_fail_despite_status(self):
        image = self.root / 'image.ppm'
        header = b'P6\n640 360\n255\n'
        hot = bytearray(bytes((2, 3, 4)) * (640 * 360))
        hot[(180 * 640 + 320) * 3:(180 * 640 + 320) * 3 + 3] = bytes((255, 100, 20))
        fixtures = {'observed_near_black': bytes((1, 4, 5)) * (640 * 360),
                    'gray': bytes((80, 80, 80)) * (640 * 360), 'hot_pixel': hot,
                    'flat_orange': bytes((220, 90, 20)) * (640 * 360)}
        for name, data in fixtures.items():
            with self.subTest(name=name):
                image.write_bytes(header + data)
                self.assertFalse(visual.inspect(image)['pass'])
        image.write_bytes(self.triangle(color=(20, 100, 220)))
        self.assertFalse(visual.inspect(image)['pass'])
        image.write_bytes(header + bytes(100))
        self.assertFalse(visual.inspect(image)['pass'])

    def test_debug_views_and_one_good_sample_cannot_supply_acceptance(self):
        stage, smoke, good = self.matrix_fixture()
        black = b'P6\n640 360\n255\n' + bytes((1, 4, 5)) * (640 * 360)
        for name in visual.NORMAL_CASES:
            # A second bad image must invalidate a seemingly good first image.
            (smoke / name / 'final-color.ppm').write_bytes(black)
        result = visual.evaluate_matrix(smoke)
        self.assertFalse(result['pass'])
        self.assertTrue(result['cases']['albedo']['pass'])
        self.assertTrue(result['cases']['emission']['pass'])
        # Passing debug captures cannot be selected as the normal control.
        (smoke / 'matrix.json').write_text(json.dumps({'schema':3,'selected_profile':'emission','translated_offset_x':1.0}))
        self.assertIn('Invalid normal profile', visual.evaluate_matrix(smoke)['error'])

    def test_lighting_success_cannot_replace_failed_emission_or_stale_controls(self):
        stage, smoke, good = self.matrix_fixture()
        black = b'P6\n640 360\n255\n' + bytes((1,4,5)) * (640*360)
        for part in ('early','final'):
            (smoke / 'emission' / (part + '-color.ppm')).write_bytes(black)
        result = visual.evaluate_matrix(smoke)
        self.assertTrue(result['lighting_path']['pass'])
        self.assertFalse(result['emission_path']['pass'])
        self.assertFalse(result['pass'])
        # Light left on cannot be called a successful off/on/off comparison.
        (smoke / 'light_off_after/final-color.ppm').write_bytes(self.triangle())
        self.assertFalse(visual.inspect_dark(smoke / 'light_off_after/final-color.ppm')['pass'])

    def test_reported_good_images_do_not_override_degenerate_uv_inputs(self):
        stage, smoke, good = self.matrix_fixture()
        (smoke / 'geometry-input.json').write_text(json.dumps({'uvs':[[0,0]]*3,'normals':[[0,0,-1]]*3}))
        result = visual.evaluate_matrix(smoke)
        self.assertTrue(result['cases']['baseline']['pass'])
        self.assertFalse(result['mesh_inputs']['pass'])
        self.assertFalse(result['lighting_path']['pass'])
        self.assertFalse(result['pass'])

    def test_emission_success_cannot_hide_lighting_control_failure(self):
        stage, smoke, good = self.matrix_fixture()
        (smoke / 'light_off_after/final-color.ppm').write_bytes(self.triangle())
        result = visual.evaluate_matrix(smoke)
        self.assertTrue(result['emission_path']['pass'])
        self.assertFalse(result['lighting_path']['pass'])
        self.assertFalse(result['pass'])

    def test_visible_captures_cannot_override_never_alpha_material(self):
        stage, smoke, good = self.matrix_fixture()
        path = smoke / 'material-input.json'
        materials = json.loads(path.read_text())
        self.assertTrue(visual.inspect_materials(smoke)['pass'])
        materials['materials'][0]['alpha_test_type'] = 0
        path.write_text(json.dumps(materials))
        result = visual.evaluate_matrix(smoke)
        self.assertTrue(result['cases']['baseline']['pass'])
        self.assertFalse(result['material_inputs']['pass'])
        self.assertFalse(result['emission_path']['pass'])
        self.assertFalse(result['lighting_path']['pass'])
        self.assertFalse(result['pass'])

    def test_sharp_output_requires_successful_configuration_for_every_case(self):
        stage, smoke, good = self.matrix_fixture()
        path = smoke / 'events.jsonl'
        events = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertTrue(visual.inspect_render_config(smoke)['pass'])
        for changed in [events[1:], events + [events[0]],
                        [events[0] | {'code':1}] + events[1:],
                        [events[0] | {'value':'True'}] + events[1:]]:
            with self.subTest(first=changed[0], length=len(changed)):
                path.write_text(''.join(json.dumps(e)+'\n' for e in changed))
                self.assertFalse(visual.inspect_render_config(smoke)['pass'])
        result = visual.evaluate_matrix(smoke)
        self.assertTrue(result['cases']['baseline']['pass'])
        self.assertFalse(result['render_config']['pass'])
        self.assertFalse(result['emission_path']['pass'])
        self.assertFalse(result['lighting_path']['pass'])
        self.assertFalse(result['pass'])

    def test_resource_warning_survives_a_successful_owned_shutdown(self):
        stage, smoke, good = self.matrix_fixture()
        (stage / 'runtime.log').write_text('[40] common device objects were not disposed of.\n')
        with patch.object(runner, 'evaluate_matrix', return_value={'pass':True}):
            passed, details = runner.smoke_evidence(stage, {'execution':'pass'})
        self.assertTrue(passed)
        self.assertEqual(len(details['resource_warnings']), 1)

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
