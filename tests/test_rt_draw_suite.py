from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import rt_draw_suite as suite
from analyze_rt_runtime import analyze as analyze_runtime
from package_rt_draw import observer_files, sha
import test_rt_draw as draw_cases


class PackageAndCompletionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        entries=observer_files()
        self.assertIn('run-performance.cmd',entries)
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

    def test_preview_marker_requires_matching_session_and_explicit_completion(self):
        marker=self.root/'preview.json'
        marker.write_text(json.dumps({'session':7,'complete':False}))
        self.assertFalse(suite.completed_preview(marker,7))
        marker.write_text(json.dumps({'session':8,'complete':True}))
        self.assertFalse(suite.completed_preview(marker,7))
        marker.write_text(json.dumps({'session':7,'complete':True}))
        self.assertTrue(suite.completed_preview(marker,7))

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

    def test_result_archive_losslessly_retains_trace_and_images_with_lzma(self):
        import zipfile
        directory=self.trace();raw=(directory/'rt-draw-host-fixture.bin').read_bytes();(directory/'world-240-1.rgba32f').write_bytes(bytes(range(256))*200)
        report={'collection':'complete','errors':[]};output=suite.archive_result(directory,report)
        with zipfile.ZipFile(output) as archive:
            self.assertIsNone(archive.testzip());self.assertEqual(archive.read('rt-draw-host-fixture.bin'),raw)
            self.assertEqual(archive.read('world-240-1.rgba32f'),bytes(range(256))*200)
            self.assertEqual(archive.getinfo('rt-draw-host-fixture.bin').compress_type,zipfile.ZIP_LZMA)
            self.assertEqual(json.loads(archive.read('omitted-files.json')),[])
            self.assertEqual(json.loads(archive.read('summary.json'))['collection'],'complete')

    def test_archive_failure_preserves_previous_valid_checkpoint_zip(self):
        import zipfile
        directory=self.trace();report={'collection':'incomplete','errors':[]}
        output=suite.archive_result(directory,report);old=output.read_bytes()
        with patch.object(suite.zipfile,'ZipFile',side_effect=OSError('fixture compression failure')):
            with self.assertRaisesRegex(OSError,'compression failure'):suite.archive_result(directory,report)
        self.assertEqual(output.read_bytes(),old)
        self.assertFalse(output.with_name(output.name+'.tmp').exists())
        with zipfile.ZipFile(output) as archive:self.assertIsNone(archive.testzip())

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

    def test_world_exit_wait_already_has_bridge_logs_and_incomplete_summary(self):
        directory=self.root/'results';directory.mkdir()
        game=self.root/'game';logs=game/'rtx-remix/logs';logs.mkdir(parents=True)
        (logs/'bridge32.log').write_text('fixture reply-deadline')
        (logs/'bridge64.log').write_text('fixture Host stopped')
        report={'collection':'incomplete','errors':[],'capture_marker':{'reason':3,'presents':33}}
        def waiting(notify):
            saved=json.loads((directory/'summary.json').read_text())
            self.assertEqual(saved['collection'],'incomplete')
            self.assertEqual(saved['collection_phase'],'waiting_for_game_and_host_exit')
            self.assertEqual(saved['pre_exit_game_logs'],'snapshot_collected')
            self.assertEqual(saved['capture_marker']['presents'],33)
            import zipfile
            with zipfile.ZipFile(directory.with_suffix('.zip')) as archive:
                self.assertEqual(json.loads(archive.read('summary.json'))['collection_phase'],'waiting_for_game_and_host_exit')
            inventory=json.loads((directory/'game-log-inventory.json').read_text())
            self.assertEqual(len(inventory['copied']),2)
            self.assertIn('reply-deadline',(directory/'game-logs/0/bridge32.log').read_text())
            raise TimeoutError('fixture game remains alive after Host exit')
        with patch.object(suite,'wait_stopped',side_effect=waiting),patch.object(suite,'analyze_world') as analyze:
            with self.assertRaisesRegex(TimeoutError,'game remains alive'):
                suite.finish_world_capture(directory,game,0,report,lambda message:None)
            analyze.assert_not_called()
        # The existing outer finally can archive this checkpoint on timeout.
        output=suite.archive_result(directory,report)
        import zipfile
        with zipfile.ZipFile(output) as archive:
            self.assertIn('game-logs/0/bridge64.log',archive.namelist())
            self.assertEqual(json.loads(archive.read('summary.json'))['collection'],'incomplete')

    def test_world_log_checkpoint_failure_does_not_prevent_final_analysis(self):
        directory=self.root/'results';directory.mkdir()
        suite.save(directory/'draw-analysis.json',{'trace':{'world_conversion':{'converted':176},
                                                          'world_normalization':'unrun_or_incomplete'}})
        report={'collection':'incomplete','errors':[]}
        with patch.object(suite,'collect_game_logs',side_effect=PermissionError('fixture locked log')),\
             patch.object(suite,'wait_stopped'),\
             patch.object(suite,'analyze_world',return_value={'runtime_outputs':'fail',
                'rt_activation_requests':1,'nonzero_world_depth_and_normals':False}):
            suite.finish_world_capture(directory,self.root/'game',0,report,lambda message:None)
        self.assertTrue((directory/'summary.json').is_file())
        self.assertTrue((directory/'world-analysis.json').is_file())
        self.assertIn('locked log',report['errors'][0])
        self.assertEqual(report['pre_exit_game_logs'],'incomplete')
        self.assertEqual(report['collection'],'incomplete')
        self.assertEqual(report['world_converted_draws'],176)
        self.assertEqual(report['rt_activation_requests'],1)

    def test_complete_draw_capture_waits_for_world_analysis_before_reporting_complete(self):
        directory=self.root/'results';directory.mkdir()
        suite.save(directory/'draw-analysis.json',{'trace':{'world_conversion':{'converted':210},
                                                          'world_normalization':'pass'}})
        report={'collection':'complete','errors':[]}
        def waiting(notify):
            self.assertEqual(json.loads((directory/'summary.json').read_text())['collection'],'incomplete')
        with patch.object(suite,'collect_game_logs'),patch.object(suite,'wait_stopped',side_effect=waiting),\
             patch.object(suite,'analyze_world',return_value={'runtime_outputs':'pass',
                'rt_activation_requests':1,'nonzero_world_depth_and_normals':True}):
            suite.finish_world_capture(directory,self.root/'game',0,report,lambda message:None)
        self.assertEqual(report['collection'],'complete')
        self.assertEqual(report['world_normalization'],'pass')

    def test_beta_completion_uses_preview_and_runtime_without_requiring_600_frame_world_analysis(self):
        directory=self.root/'beta';directory.mkdir();report={'collection':'complete','errors':[],'preview':'collected_requires_review'}
        with patch.object(suite,'collect_game_logs'),patch.object(suite,'wait_stopped'),patch.object(suite,'archive_result',return_value=directory.with_suffix('.zip')),\
             patch.object(suite,'review_preview',return_value={'pass':True,'errors':[]}),\
             patch.object(suite,'analyze_runtime',return_value={'pass':True,'errors':[],'status':'valid_evidence_requires_review'}),\
             patch.object(suite,'analyze_world') as world:
            suite.finish_world_capture(directory,self.root/'game',0,report,lambda message:None,beta=True)
        world.assert_not_called();self.assertEqual(report['collection'],'complete')
        self.assertEqual(report['beta_session'],'completed_requires_image_review')

    def test_performance_completion_uses_explicit_pilot_revision(self):
        directory=self.root/'performance-finalize';directory.mkdir()
        report={'collection':'complete','errors':[],'preview':'collected_requires_review'}
        with patch.object(suite,'collect_game_logs'),patch.object(suite,'wait_stopped'),\
             patch.object(suite,'archive_result',return_value=directory.with_suffix('.zip')),\
             patch.object(suite,'review_performance',return_value={'pass':True,'errors':[]}),\
             patch.object(suite,'analyze_runtime',return_value={'pass':True,'errors':[],'status':'valid_evidence_requires_review'}):
            suite.finish_world_capture(directory,self.root/'game',0,report,lambda message:None,
                                       performance=True,pilot_revision=46)
        self.assertEqual(report['performance'],'arc_persistent_mesh_update_collected')
        self.assertEqual(report['preview'],'completed_performance_lifecycle')
        self.assertEqual(report['collection'],'complete')

    def test_performance_review_accepts_four_exercised_phases_and_reports_fps(self):
        directory=self.root/'performance';directory.mkdir();events=[dict(event='performance_ramp_begin',code=0)]
        total=created=reused=0
        for phase,limit in enumerate((16,32,48,64)):
            submitted=min(limit,500);total+=submitted;created+=1;reused+=submitted-1
            events += [dict(event='runtime_timing_window',segment='gpu_ramp_'+str(limit),wall_ms=3000,scopes={'model':{'elapsed_ms':1000},'skin':{'elapsed_ms':500},'mesh_create':{'elapsed_ms':50},'present':{'elapsed_ms':200}}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,skinned_frame_submitted=limit,
                            skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                            preview_submitted=total,preview_skinned_submitted=total,preview_skinned_created=created,preview_skinned_reused=reused,created=created,reused=reused)]
        events.append(dict(event='preview_end',complete=True,frames=121,entries=0,pending_fences=0,held=0,submitted=total,skinned_submitted=total,gpu_skin_only_omitted=100))
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['best_fps'],10);self.assertEqual(result['worst_fps'],10)
        self.assertFalse(result['meets_24_fps'])
        events[-1]['submitted']=total+1;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_cpu_pose_submission',suite.review_performance(directory)['errors'])

    def test_performance_review_maps_v33_stale_timing_labels(self):
        directory=self.root/'performance-v33';directory.mkdir();events=[]
        total=created=reused=0
        for phase,(limit,timing) in enumerate(zip((16,32,48,64),(16,32,64,128))):
            total+=limit;created+=1;reused+=limit-1
            events += [dict(event='runtime_timing_window',segment='gpu_ramp_'+str(timing),wall_ms=3000,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                            skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                            preview_submitted=total,preview_skinned_submitted=total,preview_skinned_created=created,
                            preview_skinned_reused=reused,created=created,reused=reused)]
        events.append(dict(event='preview_end',complete=True,frames=120,submitted=total,skinned_submitted=total,
                           entries=0,pending_fences=0,held=0))
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual([p['gpu_skin_limit'] for p in result['phases']],[16,32,48,64])

    def test_performance_review_accepts_v35_fixed_arc_stability_profile(self):
        directory=self.root/'performance-v35';directory.mkdir()
        total=144;created=32;reused=112
        events=[dict(event='performance_stability_begin',code=0),
                dict(event='runtime_timing_window',segment='gpu_stability_warmup',wall_ms=2000,scopes={}),
                dict(event='api_mesh_frame',gpu_skin_performance_phase=-1,skinned_frame_limit=32,
                     preview_submitted=total,preview_skinned_submitted=total,preview_skinned_created=created,
                     preview_skinned_reused=reused)]
        for phase in range(4):
            total+=960;reused+=960
            events += [dict(event='runtime_timing_window',segment='gpu_stability_'+str(phase+1),wall_ms=1000,
                            scopes={'model':{'elapsed_ms':100},'skin':{'elapsed_ms':50},'mesh_create':{'elapsed_ms':0},'present':{'elapsed_ms':100}}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=32,
                            skinned_frame_submitted=32,skinned_frame_unique_meshes=32,skinned_frame_repeat_draws=0,
                            skinned_frame_max=32,preview_submitted=total,preview_skinned_submitted=total,
                            preview_skinned_created=created,preview_skinned_reused=reused,created=created,reused=reused)]
        events.append(dict(event='preview_end',complete=True,frames=128,entries=0,pending_fences=0,held=0,
                           submitted=total,skinned_submitted=total,gpu_skin_only_omitted=100))
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_direct_stability');self.assertEqual(result['worst_fps'],30)
        events[-1]['frames']=127;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_completion',suite.review_performance(directory)['errors'])

    def test_performance_review_v36_requires_effective_custom_importance_profile_and_thread_cpu(self):
        directory=self.root/'performance-v36';directory.mkdir();total=144;created=32;reused=112
        events=[dict(event='pilot_delivery_revision',code=0,value='36'),dict(event='performance_stability_begin',code=0),
                dict(event='runtime_timing_window',segment='gpu_stability_warmup',wall_ms=2000,
                     thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                dict(event='api_mesh_frame',gpu_skin_performance_phase=-1,skinned_frame_limit=32,
                     preview_submitted=total,preview_skinned_submitted=total,preview_skinned_created=created,preview_skinned_reused=reused)]
        for phase in range(4):
            total+=960;reused+=960
            events += [dict(event='runtime_timing_window',segment='gpu_stability_'+str(phase+1),wall_ms=1000,
                            thread_times_available=True,thread_kernel_ms=50,thread_user_ms=150,thread_cpu_ms=200,
                            utc_filetime_100ns=116444736000000000+(phase+2)*10_000_000,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=32,
                            skinned_frame_submitted=32,skinned_frame_unique_meshes=32,skinned_frame_repeat_draws=0,
                            skinned_frame_max=32,preview_submitted=total,preview_skinned_submitted=total,
                            preview_skinned_created=created,preview_skinned_reused=reused,created=created,reused=reused)]
        events.append(dict(event='preview_end',complete=True,frames=128,entries=0,pending_fences=0,held=0,
                           submitted=total,skinned_submitted=total,gpu_skin_only_omitted=100))
        path=directory/'preview-runtime.jsonl';path.write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='0')])+'\n')
        (directory/'gpu-engine-samples.jsonl').write_text('\n'.join(json.dumps({'unix_ns':(phase+2)*1_000_000_000,'engines':[],
            'process_cpu':[{'pid':7,'role':'host','interval_begin_unix_ns':(phase+1)*1_000_000_000,'cpu_ms':100}]}) for phase in range(4)))
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: Importance Sampled - activated\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_direct_custom_stability');self.assertEqual(result['phases'][0]['host_device_thread_non_cpu_ms'],800)
        self.assertEqual(result['phases'][0]['process_cpu'][0]['one_core_percent'],10)
        (directory/'remix-dxvk.log').write_text('[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        self.assertIn('effective_indirect_mode_not_importance_sampled',suite.review_performance(directory)['errors'])

    def test_performance_review_v38_requires_hybrid_fallback_recovery_and_raster_survival(self):
        directory=self.root/'performance-v38';directory.mkdir();total=created=reused=0
        events=[dict(event='pilot_delivery_revision',code=0,value='38'),dict(event='performance_lifecycle_begin',code=0)]
        for phase,(limit,frames,name) in enumerate(((0,30,'gpu_lifecycle_native_fallback'),(8,60,'gpu_lifecycle_8'),(16,120,'gpu_lifecycle_16'))):
            submitted=limit*frames;total+=submitted
            if submitted:created+=limit;reused+=submitted-limit
            events += [dict(event='runtime_timing_window',segment=name,wall_ms=frames*50,
                            thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                            skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                            skinned_frame_max=16,preview_submitted=total,preview_skinned_submitted=total,
                            preview_skinned_created=created,preview_skinned_reused=reused,created=created,reused=reused)]
        events += [dict(event='performance_rt_recovery_requested',code=0),dict(event='performance_resources_recovered',code=0),
                   dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,
                        thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=271,rt_frames=210,raster_survival_frames=60,
                        resources_recovered=True,original_fallback_draws=1000,entries=0,pending_fences=0,held=0,submitted=total,skinned_submitted=total)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='True')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_restir_hybrid_lifecycle');self.assertEqual([p['gpu_skin_limit'] for p in result['phases']],[0,8,16])
        events[-1]['raster_survival_frames']=59;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_lifecycle_recovery',suite.review_performance(directory)['errors'])
        events[-1]['raster_survival_frames']=60;events[-1]['original_fallback_draws']=0;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_original_fallback_not_exercised',suite.review_performance(directory)['errors'])

    def test_performance_review_v39_requires_complete_cpu_pose_fallback_in_every_phase(self):
        directory=self.root/'performance-v39';directory.mkdir();total=skinned=created=reused=cpu=0
        events=[dict(event='pilot_delivery_revision',code=0,value='39'),dict(event='performance_lifecycle_begin',code=0)]
        for phase,(limit,name) in enumerate(((0,'gpu_lifecycle_cpu_complete'),(8,'gpu_lifecycle_8'),(16,'gpu_lifecycle_16'))):
            gpu=limit*30;cpu_phase=20*30;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase
            if gpu:created+=limit;reused+=gpu-limit
            events += [dict(event='runtime_timing_window',segment=name,wall_ms=1500,
                            thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                            skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                            skinned_frame_max=16,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                            preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                            created=total,reused=0)]
        events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,
                        thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=150,rt_frames=90,raster_survival_frames=60,
                        resources_recovered=True,cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,
                        submitted=total,skinned_submitted=skinned)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='False')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n[RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled.\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_restir_complete_lifecycle')
        self.assertEqual([p['cpu_pose_submitted_delta'] for p in result['phases']],[600,600,600])
        events[-1]['cpu_pose_submitted']=0;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_complete_cpu_pose_fallback',suite.review_performance(directory)['errors'])

    def test_performance_review_v40_requires_all_scaling_phases(self):
        directory=self.root/'performance-v40';directory.mkdir();total=skinned=created=reused=cpu=0
        events=[dict(event='pilot_delivery_revision',code=0,value='40'),dict(event='performance_lifecycle_begin',code=0)]
        for phase,limit in enumerate((16,64,128)):
            gpu=limit*30;cpu_phase=20*30;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase;created+=limit;reused+=gpu-limit
            events += [dict(event='runtime_timing_window',segment='gpu_lifecycle_'+str(limit),wall_ms=1500,
                            thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                            skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                            skinned_frame_max=128,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                            preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                            created=total,reused=0)]
        events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,
                        thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=150,rt_frames=90,raster_survival_frames=60,
                        resources_recovered=True,cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,
                        submitted=total,skinned_submitted=skinned)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='False')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual([p['gpu_skin_limit'] for p in result['phases']],[16,64,128])
        self.assertEqual(result['phases'][0]['gpu_skin_submitted_delta'],480)

    def test_performance_review_v41_requires_three_fixed_64_windows(self):
        directory=self.root/'performance-v41';directory.mkdir();total=skinned=created=reused=cpu=0
        events=[dict(event='pilot_delivery_revision',code=0,value='41'),dict(event='performance_lifecycle_begin',code=0)]
        for phase,name in enumerate(('cpu_pose_fast_1','cpu_pose_fast_2','cpu_pose_fast_3')):
            gpu=64*30;cpu_phase=20*30;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase;created+=64;reused+=gpu-64
            events += [dict(event='runtime_timing_window',segment=name,wall_ms=1500,
                            thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=64,
                            skinned_frame_submitted=64,skinned_frame_unique_meshes=64,skinned_frame_repeat_draws=0,
                            skinned_frame_max=64,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                            preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                            created=total,reused=0)]
        events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,
                        thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=150,rt_frames=90,raster_survival_frames=60,
                        resources_recovered=True,cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,
                        submitted=total,skinned_submitted=skinned)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='False')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual([p['gpu_skin_limit'] for p in result['phases']],[64,64,64])
        self.assertIn('v41 fixes audited persistent GPU skinning',result['limits'])
        events[3]['skinned_frame_limit']=128;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_phase_sequence',suite.review_performance(directory)['errors'])

    def test_performance_review_v42_requires_optimized_64_80_96_phases(self):
        directory=self.root/'performance-v42';directory.mkdir();total=skinned=created=reused=cpu=0
        events=[dict(event='pilot_delivery_revision',code=0,value='42'),dict(event='performance_lifecycle_begin',code=0)]
        for phase,(limit,name) in enumerate(zip((64,80,96),('gpu_optimized_64','gpu_optimized_80','gpu_optimized_96'))):
            gpu=limit*30;cpu_phase=20*30;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase;created+=limit;reused+=gpu-limit
            events += [dict(event='runtime_timing_window',segment=name,wall_ms=1500,
                            thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                       dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                            skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                            skinned_frame_max=96,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                            preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                            created=total,reused=0)]
        events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,
                        thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=150,rt_frames=90,raster_survival_frames=60,
                        resources_recovered=True,cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,
                        submitted=total,skinned_submitted=skinned)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='False')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual([p['gpu_skin_limit'] for p in result['phases']],[64,80,96])
        self.assertIn('v42 retains the optimized',result['limits'])
        events[5]['skinned_frame_max']=128;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_gpu_skin_not_exercised',suite.review_performance(directory)['errors'])

    def test_performance_review_v43_requires_parallel_lossless_frame_batch(self):
        directory=self.root/'performance-v43';directory.mkdir()
        events=[dict(event='pilot_delivery_revision',code=0,value='43'),dict(event='performance_lifecycle_begin',code=0)]
        events += [dict(event='runtime_timing_window',segment='explicit_oracle_80',wall_ms=6000,thread_times_available=True,
                       thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='api_mesh_frame',stage='api_continuous_preview',gpu_skin_performance_phase=0,skinned_frame_limit=80,
                        skinned_frame_submitted=80,skinned_frame_unique_meshes=80,skinned_frame_repeat_draws=0,skinned_frame_max=80,
                        preview_submitted=3000,preview_skinned_submitted=2400,skinned_submitted=2400,skinned_created=80,skinned_reused=2320)]
        worker=0
        for frame in range(60):
            worker+=100;events.append(dict(event='frame_batch_flush',frame=100+frame,source_draws=100,source_vertices=1000,
                source_indices=3000,batches=10,submitted_source_draws=100,worker_submitted=worker,worker_completed=worker,
                worker_failed=0,worker_threads=4,worker_max_active=4,worker_cpu_elapsed_ms=frame+1.0))
            if frame in (29,59):
                phase=1 if frame==29 else 2;name='frame_batch_warm' if phase==1 else 'frame_batch_steady'
                events += [dict(event='runtime_timing_window',segment=name,wall_ms=3000,thread_times_available=True,
                                thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,
                                scopes={'batch_wait':{'calls':30,'elapsed_ms':100,'max_ms':5},'batch_assemble':{'calls':30,'elapsed_ms':50,'max_ms':3},'batch_submit':{'calls':300,'elapsed_ms':200,'max_ms':2}}),
                           dict(event='api_mesh_frame',stage='api_continuous_preview',gpu_skin_performance_phase=phase,skinned_frame_limit=0,
                                skinned_frame_submitted=0,skinned_frame_unique_meshes=0,skinned_frame_repeat_draws=0,skinned_frame_max=80,
                                preview_submitted=3000+phase*300,preview_skinned_submitted=2400,
                                skinned_submitted=2400,skinned_created=80,skinned_reused=2320)]
        events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,thread_times_available=True,
                        thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=150,rt_frames=90,raster_survival_frames=60,resources_recovered=True,
                        entries=0,pending_fences=0,held=0,submitted=3600,skinned_submitted=2400,skinned_created=80,skinned_reused=2320,
                        budget_omitted=0,frame_batch_flushes=60,frame_batch_source_draws=6000,frame_batch_submitted_source_draws=6000,
                        frame_batch_failures=0,frame_batch_worker_submitted=6000,frame_batch_worker_completed=6000,
                        frame_batch_worker_failed=0,frame_batch_worker_threads=4,frame_batch_worker_max_active=4,persistent_mesh_updates=0)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='False')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_structural_frame_batch');self.assertEqual(result['frame_batch']['compression_ratio'],10)
        self.assertTrue(analyze_runtime(directory)['pass'])
        events[0]['value']='44'
        flushes=[e for e in events if e.get('event')=='frame_batch_flush']
        for index,row in enumerate(flushes):
            row['persistent_phase']=index>=30
            row['persistent_created']=10 if index==30 else 0
            row['persistent_updated']=10 if index>30 else 0
        events[-1]['persistent_mesh_updates']=290
        next(e for e in events if e.get('segment')=='frame_batch_steady')['segment']='persistent_batch_update'
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_persistent_mesh_update');self.assertTrue(analyze_runtime(directory)['pass'])
        self.assertAlmostEqual(result['persistent_vs_warm']['fps_gain_percent'],0)
        self.assertFalse(result['persistent_vs_warm']['material_gain'])
        flushes[31]['persistent_created']=1;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_persistent_mesh_update',suite.review_performance(directory)['errors'])
        self.assertIn('invalid_structural_frame_batch',analyze_runtime(directory)['errors']);flushes[31]['persistent_created']=0
        events[-1]['frame_batch_worker_completed']=5999;(directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_frame_batch_accounting',suite.review_performance(directory)['errors'])
        self.assertIn('invalid_structural_frame_batch',analyze_runtime(directory)['errors'])

    def test_performance_review_v48_requires_function_hook_and_reused_resources_without_cpu_batch(self):
        directory=self.root/'performance-v48';directory.mkdir()
        events=[dict(event='pilot_delivery_revision',code=0,value='48'),dict(event='performance_lifecycle_begin',code=0)]
        for instance in range(1,10):
            events.append(dict(event='model_resource_scope_submitted',frame=instance,scope=100+instance,
                resource=1 if instance%2 else 2,instance=1000+instance,surfaces=4,bones=32,created=instance<=2))
        for phase,(name,instances) in enumerate(zip(('model_resource_warm','model_resource_steady','model_resource_reuse'),(3,6,9))):
            events += [dict(event='runtime_timing_window',segment=name,wall_ms=3000,thread_times_available=True,
                            thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={
                                'model':{'calls':30,'elapsed_ms':100,'max_ms':5},
                                'prepare':{'calls':30,'elapsed_ms':50,'max_ms':3},
                                'mesh_create':{'calls':2 if phase==0 else 0,'elapsed_ms':20 if phase==0 else 0,'max_ms':10 if phase==0 else 0},
                                'instance':{'calls':3,'elapsed_ms':15,'max_ms':5}}),
                       dict(event='api_mesh_frame',stage='api_continuous_preview',gpu_skin_performance_phase=phase,
                            skinned_frame_limit=0,skinned_frame_submitted=3,skinned_frame_unique_meshes=2,
                            skinned_frame_repeat_draws=1,skinned_frame_max=80,preview_submitted=instances,
                            preview_skinned_submitted=instances,preview_skinned_created=2,preview_skinned_reused=instances-2,
                            skinned_submitted=instances,skinned_created=2,skinned_reused=instances-2,
                            model_resource_scopes=instances,model_resource_definitions=2,
                            model_resource_instances=instances,model_resource_failures=0)]
        events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                   dict(event='runtime_timing_window',segment='gpu_lifecycle_raster',wall_ms=1000,thread_times_available=True,
                        thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30,scopes={}),
                   dict(event='preview_end',complete=True,frames=150,rt_frames=90,raster_survival_frames=60,
                        resources_recovered=True,entries=0,pending_fences=0,held=0,submitted=9,skinned_submitted=9,
                        skinned_created=2,skinned_reused=7,cpu_pose_submitted=0,cpu_pose_created=0,cpu_pose_reused=0,
                        budget_omitted=0,frame_batch_flushes=0,frame_batch_source_draws=0,
                        frame_batch_submitted_source_draws=0,frame_batch_failures=0,frame_batch_worker_submitted=0,
                        frame_batch_worker_completed=0,frame_batch_worker_failed=0,frame_batch_worker_threads=0,
                        frame_batch_worker_max_active=0,model_resource_scopes=9,model_resource_definitions=2,
                        model_resource_instances=9,model_resource_failures=0)]
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        (directory/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in
            [dict(event='rtx.graphicsPreset',code=0,value='4'),dict(event='rtx.integrateIndirectMode',code=0,value='1'),
             dict(event='rtx.useVertexCapture',code=0,value='False')])+'\n')
        (directory/'remix-dxvk.log').write_text('rtx.graphicsPreset = 4\n[RTX] Integrate Indirect Mode: ReSTIR GI - activated\n')
        controls=[dict(event='model_hook_status',value=1,other=0x12340000),
                  dict(event='model_hook_capture_start',value=10,other=5),
                  dict(event='model_hook_capture_end',value=110,other=95),
                  dict(event='model_hook_rejected',value=15,other=0)]
        control=directory/'rt-draw-control-1.jsonl';control.write_text('\n'.join(json.dumps(e) for e in controls))
        result=suite.review_performance(directory);self.assertTrue(result['pass'],result['errors'])
        self.assertEqual(result['profile'],'arc_model_resource_instance');self.assertEqual(result['model_resources']['model_resource_definitions'],2)
        self.assertEqual(result['model_hook']['captured_delta'],90)
        runtime=analyze_runtime(directory);self.assertTrue(runtime['pass'],runtime['errors'])
        self.assertEqual(runtime['gpu_skinning']['status'],'performance_model_resource_requires_visual_review')
        control.unlink();self.assertIn('performance_model_hook_evidence',suite.review_performance(directory)['errors']);control.write_text('\n'.join(json.dumps(e) for e in controls))
        events[-1]['cpu_pose_submitted']=1
        (directory/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
        self.assertIn('performance_model_resource_instance',suite.review_performance(directory)['errors'])
        self.assertIn('invalid_model_resource_instance_lifecycle',analyze_runtime(directory)['errors'])


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
