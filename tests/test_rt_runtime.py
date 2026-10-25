from pathlib import Path
import json
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_runtime import analyze
from rt_gpu_telemetry import select_rows,process_cpu_delta,Sampler,PdhQuery

class RuntimeEvidenceTests(unittest.TestCase):
    def test_pdh_invalid_data_is_a_transient_sample_gap(self):
        self.assertTrue(PdhQuery.transient(0xC0000BBA))
        self.assertTrue(PdhQuery.transient(-2147481643))
        self.assertFalse(PdhQuery.transient(0xC0000BB8))
    def test_process_and_status_binding_preserves_separate_engines(self):
        rows=[('pid_12_luid_A_eng_0_engtype_3D',0,70.),('pid_12_luid_A_eng_1_engtype_Compute',1,65.),
              ('pid_120_luid_A_eng_0',0,90.),('pid_12_bad',2,99.),('pid_12_nan',0,float('nan')),('pid_12_over',0,101.),('pid_12_negative',0,-1.)]
        result=select_rows(rows,{12})
        self.assertEqual([x['percent'] for x in result],[70.,65.]);self.assertEqual(sum(x['percent'] for x in result),135.)
        self.assertEqual(len(select_rows(rows*200,{12})),128)
    def test_process_cpu_delta_keeps_one_core_and_machine_scales_distinct(self):
        result=process_cpu_delta((10,100,200),(10,100100,100200),1_000_000_000,8)
        self.assertEqual(result['cpu_ms'],20);self.assertEqual(result['one_core_percent'],2)
        self.assertEqual(result['machine_percent'],.25)
        self.assertIsNone(process_cpu_delta((10,100,200),(11,100100,200100),1_000_000_000,8))
    @unittest.skipIf(sys.platform=='win32','Non-Windows unavailable path only')
    def test_optional_counter_failure_records_status_and_stops(self):
        with tempfile.TemporaryDirectory() as d:
            sampler=Sampler(d).start();self.assertTrue(sampler.stop())
            status=json.loads((Path(d)/'gpu-engine-status.json').read_text())
            self.assertEqual(status['status'],'unavailable');self.assertEqual(status['samples'],0)
    def test_native_edge_pair_and_queued_config_are_separate_from_image_acceptance(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            events=[dict(event='pilot_delivery_revision',value='24'),dict(event='runtime_timing_window',wall_ms=10,segment='preview_no_aa',scopes={'model':dict(calls=1,elapsed_ms=5,max_ms=5),'prepare':dict(calls=1,elapsed_ms=4,max_ms=4)}),
                    dict(event='native_taa_requested',scale_code=0,taa_code=0,resolution_scale=1,upscaler_type=3)]
            for mode in ('no-aa','taa'):
                file=f'native-{mode}.bgra8';(root/file).write_bytes(bytes(range(16)))
                events.append(dict(event='native_edge_readback',mode=mode,file=file,width=2,height=2,source_width=4,source_height=4,x=1,y=1,resampled=False,copy_code=0,hresult=0,written=True))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            value=review();self.assertTrue(value['pass']);self.assertEqual(value['native_edge_evidence'],'complete_requires_image_review')
            self.assertEqual(value['gpu_engine_collection']['status'],'unavailable')
            events[-1]['file']='../outside.bgra8';self.assertEqual(review()['native_edge_evidence'],'unavailable_or_incomplete')
            events[-1]['file']='native-taa.bgra8';events[-1]['width']=3
            self.assertEqual(review()['native_edge_evidence'],'unavailable_or_incomplete')
            events[2]['resolution_scale']=.75;self.assertIn('missing_or_failed_native_taa_request',review()['errors'])
            events[2]['resolution_scale']=1;events[1]['scopes']['model']['max_ms']=float('nan')
            self.assertIn('invalid_timing_counter',review()['errors'])
    def test_older_session_is_not_subject_to_new_quality_requirements(self):
        with tempfile.TemporaryDirectory() as d:self.assertEqual(analyze(d)['status'],'not_requested')
    def test_v25_requires_exercised_and_accounted_persistent_skinning(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='25'),
                dict(event='runtime_timing_window',wall_ms=1,segment='preview_no_aa',scopes={}),
                dict(event='native_taa_requested',scale_code=0,taa_code=0,resolution_scale=1,upscaler_type=3),
                dict(event='api_mesh_frame',skinned_submitted=10,skinned_created=2,skinned_reused=8),
                dict(event='preview_end',skinned_submitted=10,skinned_created=2,skinned_reused=8)]
            for mode in ('no-aa','taa'):
                file=f'native-{mode}.bgra8';(root/file).write_bytes(bytes(16));events.append(dict(event='native_edge_readback',mode=mode,file=file,width=2,height=2,source_width=2,source_height=2,x=0,y=0,resampled=False,copy_code=0,hresult=0,written=True))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_skinning']['status'],'exercised_requires_image_review')
            events[4]['skinned_reused']=7;self.assertIn('invalid_gpu_skinning_accounting',review()['errors'])
            events[4]['skinned_reused']=8;events[4]['skinned_submitted']=0;self.assertIn('gpu_skinning_not_exercised',review()['errors'])
    def test_v27_requires_gpu_skinning_to_remain_isolated(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='27'),
                dict(event='runtime_timing_window',wall_ms=1,segment='preview_no_aa',scopes={}),
                dict(event='native_taa_requested',scale_code=0,taa_code=0,resolution_scale=1,upscaler_type=3),
                dict(event='api_mesh_frame',skinned_submitted=0,skinned_created=0,skinned_reused=0),
                dict(event='preview_end',skinned_submitted=0,skinned_created=0,skinned_reused=0)]
            for mode in ('no-aa','taa'):
                file=f'native-{mode}.bgra8';(root/file).write_bytes(bytes(16));events.append(dict(event='native_edge_readback',mode=mode,file=file,width=2,height=2,source_width=2,source_height=2,x=0,y=0,resampled=False,copy_code=0,hresult=0,written=True))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_skinning']['status'],'disabled_cpu_fallback')
            events[3]['skinned_submitted']=events[3]['skinned_created']=1
            self.assertIn('gpu_skinning_safety_rollback_not_effective',review()['errors'])
    def test_v28_requires_exercised_bounded_gpu_skin_canary(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='28'),
                dict(event='runtime_timing_window',wall_ms=1,segment='preview_no_aa',scopes={}),
                dict(event='native_taa_requested',scale_code=0,taa_code=0,resolution_scale=1,upscaler_type=3),
                dict(event='api_mesh_frame',skinned_submitted=16,skinned_created=2,skinned_reused=14,skinned_frame_submitted=16,skinned_frame_limit=16),
                dict(event='preview_end',skinned_submitted=16,skinned_created=2,skinned_reused=14)]
            for mode in ('no-aa','taa'):
                file=f'native-{mode}.bgra8';(root/file).write_bytes(bytes(16));events.append(dict(event='native_edge_readback',mode=mode,file=file,width=2,height=2,source_width=2,source_height=2,x=0,y=0,resampled=False,copy_code=0,hresult=0,written=True))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_skinning']['status'],'bounded_canary_requires_image_review')
            events[3]['skinned_frame_submitted']=17;self.assertIn('gpu_skinning_canary_frame_limit',review()['errors'])
            events[3]['skinned_frame_submitted']=16;events[4].update(skinned_submitted=0,skinned_created=0,skinned_reused=0)
            self.assertIn('gpu_skinning_canary_not_exercised',review()['errors'])
    def test_v29_promotes_the_same_bound_to_a_safety_limit(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='29'),
                dict(event='runtime_timing_window',wall_ms=1,segment='preview_no_aa',scopes={}),
                dict(event='native_taa_requested',scale_code=0,taa_code=0,resolution_scale=1,upscaler_type=3),
                dict(event='api_mesh_frame',skinned_submitted=16,skinned_created=2,skinned_reused=14,skinned_frame_submitted=16,skinned_frame_limit=16),
                dict(event='preview_end',skinned_submitted=16,skinned_created=2,skinned_reused=14)]
            for mode in ('no-aa','taa'):
                file=f'native-{mode}.bgra8';(root/file).write_bytes(bytes(16));events.append(dict(event='native_edge_readback',mode=mode,file=file,width=2,height=2,source_width=2,source_height=2,x=0,y=0,resampled=False,copy_code=0,hresult=0,written=True))
            (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            result=analyze(root);self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_skinning']['status'],'safety_limit_requires_image_review')
    def test_v30_retains_a_bounded_gpu_baseline_in_high_density_frames(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='30'),
                dict(event='runtime_timing_window',wall_ms=1,segment='preview_no_aa',scopes={}),
                dict(event='native_taa_requested',scale_code=0,taa_code=0,resolution_scale=1,upscaler_type=3),
                dict(event='api_mesh_frame',skinned_submitted=4,skinned_created=1,skinned_reused=3,skinned_frame_submitted=4,skinned_frame_limit=4,skinned_frame_max=16,
                     gpu_skin_previous_frame_draws=629,gpu_skin_density_limit=256,gpu_skin_high_density_limit=4,gpu_skin_density_throttle_frames=59),
                dict(event='preview_end',skinned_submitted=4,skinned_created=1,skinned_reused=3,gpu_skin_density_throttle_frames=64)]
            for mode in ('no-aa','taa'):
                file=f'native-{mode}.bgra8';(root/file).write_bytes(bytes(16));events.append(dict(event='native_edge_readback',mode=mode,file=file,width=2,height=2,source_width=2,source_height=2,x=0,y=0,resampled=False,copy_code=0,hresult=0,written=True))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_skinning']['status'],'density_adaptive_requires_image_review')
            events[3]['skinned_frame_submitted']=5
            self.assertIn('invalid_gpu_skinning_density_policy',review()['errors'])
    def test_v33_performance_ramp_requires_all_four_exercised_limits_without_taa(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='33'),dict(event='performance_ramp_begin',code=0)]
            total=created=reused=0
            for phase,limit in enumerate((16,32,48,64)):
                submitted=min(limit,500);total+=submitted;created+=1;reused+=submitted-1
                events += [dict(event='runtime_timing_window',wall_ms=2500,segment='gpu_ramp_'+str(limit),scopes={}),
                    dict(event='api_mesh_frame',skinned_submitted=total,skinned_created=created,skinned_reused=reused,
                         preview_submitted=total,preview_skinned_submitted=total,preview_skinned_created=created,preview_skinned_reused=reused,
                         skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                         skinned_frame_limit=limit,skinned_frame_max=64,gpu_skin_performance_phase=phase)]
            events.append(dict(event='preview_end',submitted=total,skinned_submitted=total,skinned_created=created,skinned_reused=reused,gpu_skin_only_omitted=100,gpu_skin_density_throttle_frames=0))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['gpu_skinning']['status'],'performance_ramp_requires_review')
            events[7]['skinned_frame_limit']=47
            self.assertIn('invalid_gpu_skinning_performance_ramp',review()['errors'])
            events[7]['skinned_frame_limit']=48;events[-1]['submitted']=total+1
            self.assertIn('performance_cpu_pose_submission',review()['errors'])

    def test_performance_ignores_diagnostic_api_mesh_frames(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='34'),dict(event='performance_ramp_begin',code=0),
                dict(event='api_mesh_frame',frame=30,skinned_submitted=0,skinned_created=0,skinned_reused=0)]
            total=created=reused=0
            for phase,limit in enumerate((16,32,48,64)):
                total+=limit;created+=1;reused+=limit-1
                events += [dict(event='runtime_timing_window',wall_ms=2500,segment='gpu_ramp_'+str(limit),scopes={}),
                    dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                         skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                         skinned_frame_max=64,preview_submitted=total,preview_skinned_submitted=total,
                         preview_skinned_created=created,preview_skinned_reused=reused,skinned_submitted=total,
                         skinned_created=created,skinned_reused=reused)]
            events.append(dict(event='preview_end',submitted=total,skinned_submitted=total,
                               skinned_created=created,skinned_reused=reused))
            (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            result=analyze(root);self.assertTrue(result['pass'],result['errors'])
            self.assertEqual(result['gpu_skinning']['mesh_frames'],4)

    def test_v35_arc_stability_requires_warmup_and_four_fixed_32_windows(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='35'),
                dict(event='performance_stability_begin',code=0),
                dict(event='runtime_timing_window',wall_ms=1000,segment='gpu_stability_warmup',scopes={}),
                dict(event='api_mesh_frame',gpu_skin_performance_phase=-1,skinned_frame_limit=32,
                     preview_submitted=144,preview_skinned_submitted=144,preview_skinned_created=32,
                     preview_skinned_reused=112,skinned_submitted=144,skinned_created=32,skinned_reused=112)]
            total=144;created=32;reused=112
            for phase in range(4):
                total+=960;reused+=960
                events += [dict(event='runtime_timing_window',wall_ms=1200,segment='gpu_stability_'+str(phase+1),scopes={}),
                    dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=32,
                         skinned_frame_submitted=32,skinned_frame_unique_meshes=32,skinned_frame_repeat_draws=0,
                         skinned_frame_max=32,preview_submitted=total,preview_skinned_submitted=total,
                         preview_skinned_created=created,preview_skinned_reused=reused,skinned_submitted=total,
                         skinned_created=created,skinned_reused=reused)]
            events.append(dict(event='preview_end',submitted=total,skinned_submitted=total,
                               skinned_created=created,skinned_reused=reused))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors'])
            self.assertEqual(result['gpu_skinning']['status'],'performance_stability_requires_review')
            events[0]['value']='36'
            for event in events:
                if event.get('event')=='runtime_timing_window':event.update(thread_times_available=True,thread_kernel_ms=10,thread_user_ms=20,thread_cpu_ms=30)
            result=review();self.assertTrue(result['pass'],result['errors'])
            self.assertEqual(result['window_timeline'][0]['thread_cpu_ms'],30)
            events[2]['thread_times_available']=False
            self.assertIn('invalid_thread_cpu_timing',review()['errors'])
            events[0]['value']='35';events[2]['thread_times_available']=True
            events[5]['skinned_frame_max']=64
            (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            self.assertIn('invalid_gpu_skinning_performance_ramp',analyze(root)['errors'])

    def test_v38_lifecycle_requires_bounded_explicit_load_native_fallback_and_raster_survival(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='38'),dict(event='performance_lifecycle_begin',code=0)]
            total=created=reused=0
            for phase,(limit,frames,name) in enumerate(((0,30,'gpu_lifecycle_native_fallback'),(8,60,'gpu_lifecycle_8'),(16,120,'gpu_lifecycle_16'))):
                submitted=limit*frames;total+=submitted
                if submitted:created+=limit;reused+=submitted-limit
                events += [dict(event='runtime_timing_window',wall_ms=frames*50,segment=name,scopes={},
                                thread_times_available=True,thread_cpu_ms=10),
                    dict(event='api_mesh_frame',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                         skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                         skinned_frame_max=16,preview_submitted=total,preview_skinned_submitted=total,
                         preview_skinned_created=created,preview_skinned_reused=reused,skinned_submitted=total,
                         skinned_created=created,skinned_reused=reused)]
            events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                dict(event='runtime_timing_window',wall_ms=1000,segment='gpu_lifecycle_raster',scopes={},thread_times_available=True,thread_cpu_ms=10),
                dict(event='preview_end',complete=True,rt_frames=210,raster_survival_frames=60,resources_recovered=True,
                     original_fallback_draws=1000,entries=0,pending_fences=0,held=0,submitted=total,skinned_submitted=total,skinned_created=created,skinned_reused=reused)]
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors'])
            self.assertEqual(result['gpu_skinning']['status'],'performance_hybrid_lifecycle_requires_visual_review')
            events[-1]['resources_recovered']=False
            self.assertIn('invalid_gpu_skinning_performance_ramp',review()['errors'])

    def test_v39_lifecycle_requires_complete_explicit_cpu_pose_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='39'),dict(event='performance_lifecycle_begin',code=0)]
            total=skinned=created=reused=cpu=0
            for phase,(limit,name) in enumerate(((0,'gpu_lifecycle_cpu_complete'),(8,'gpu_lifecycle_8'),(16,'gpu_lifecycle_16'))):
                gpu=limit*30;cpu_phase=600;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase
                if gpu:created+=limit;reused+=gpu-limit
                events += [dict(event='runtime_timing_window',wall_ms=1500,segment=name,scopes={},thread_times_available=True,thread_cpu_ms=10),
                    dict(event='api_mesh_frame',stage='api_continuous_preview',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                         skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                         skinned_frame_max=16,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                         preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                         skinned_submitted=skinned,skinned_created=created,skinned_reused=reused)]
            events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                dict(event='runtime_timing_window',wall_ms=1000,segment='gpu_lifecycle_raster',scopes={},thread_times_available=True,thread_cpu_ms=10),
                dict(event='preview_end',complete=True,rt_frames=90,raster_survival_frames=60,resources_recovered=True,
                     cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,submitted=total,skinned_submitted=skinned,skinned_created=created,skinned_reused=reused)]
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors'])
            self.assertEqual(result['gpu_skinning']['status'],'performance_complete_lifecycle_requires_visual_review')
            events[-1]['cpu_pose_submitted']=0
            self.assertIn('performance_complete_cpu_pose_fallback',review()['errors'])

    def test_v40_lifecycle_scales_gpu_skin_with_complete_cpu_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='40'),dict(event='performance_lifecycle_begin',code=0)]
            total=skinned=created=reused=cpu=0
            for phase,limit in enumerate((16,64,128)):
                gpu=limit*30;cpu_phase=600;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase
                created+=limit;reused+=gpu-limit
                events += [dict(event='runtime_timing_window',wall_ms=1500,segment='gpu_lifecycle_'+str(limit),scopes={},thread_times_available=True,thread_cpu_ms=10),
                    dict(event='api_mesh_frame',stage='api_continuous_preview',gpu_skin_performance_phase=phase,skinned_frame_limit=limit,
                         skinned_frame_submitted=limit,skinned_frame_unique_meshes=limit,skinned_frame_repeat_draws=0,
                         skinned_frame_max=128,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                         preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                         skinned_submitted=skinned,skinned_created=created,skinned_reused=reused)]
            events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                dict(event='runtime_timing_window',wall_ms=1000,segment='gpu_lifecycle_raster',scopes={},thread_times_available=True,thread_cpu_ms=10),
                dict(event='preview_end',complete=True,rt_frames=90,raster_survival_frames=60,resources_recovered=True,
                     cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,
                     submitted=total,skinned_submitted=skinned,skinned_created=created,skinned_reused=reused)]
            (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            result=analyze(root);self.assertTrue(result['pass'],result['errors'])
            self.assertEqual(result['gpu_skinning']['status'],'performance_complete_lifecycle_requires_visual_review')
            events[3]['skinned_frame_limit']=63
            (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            self.assertIn('invalid_gpu_skinning_performance_ramp',analyze(root)['errors'])

    def test_v41_lifecycle_holds_gpu_64_for_cpu_pose_fast_path(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[dict(event='pilot_delivery_revision',value='41'),dict(event='performance_lifecycle_begin',code=0)]
            total=skinned=created=reused=cpu=0
            for phase,name in enumerate(('cpu_pose_fast_1','cpu_pose_fast_2','cpu_pose_fast_3')):
                gpu=64*30;cpu_phase=600;skinned+=gpu;cpu+=cpu_phase;total+=gpu+cpu_phase
                created+=64;reused+=gpu-64
                events += [dict(event='runtime_timing_window',wall_ms=1500,segment=name,scopes={},thread_times_available=True,thread_cpu_ms=10),
                    dict(event='api_mesh_frame',stage='api_continuous_preview',gpu_skin_performance_phase=phase,skinned_frame_limit=64,
                         skinned_frame_submitted=64,skinned_frame_unique_meshes=64,skinned_frame_repeat_draws=0,
                         skinned_frame_max=64,preview_submitted=total,preview_cpu_pose_submitted=cpu,preview_cpu_pose_created=cpu,preview_cpu_pose_reused=0,
                         preview_skinned_submitted=skinned,preview_skinned_created=created,preview_skinned_reused=reused,
                         skinned_submitted=skinned,skinned_created=created,skinned_reused=reused)]
            events += [dict(event='performance_resources_recovered',code=0),dict(event='performance_raster_survival_complete',code=0),
                dict(event='runtime_timing_window',wall_ms=1000,segment='gpu_lifecycle_raster',scopes={},thread_times_available=True,thread_cpu_ms=10),
                dict(event='preview_end',complete=True,rt_frames=90,raster_survival_frames=60,resources_recovered=True,
                     cpu_pose_submitted=cpu,cpu_pose_created=cpu,cpu_pose_reused=0,budget_omitted=0,entries=0,pending_fences=0,held=0,
                     submitted=total,skinned_submitted=skinned,skinned_created=created,skinned_reused=reused)]
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
            result=review();self.assertTrue(result['pass'],result['errors'])
            self.assertIn('v41 fixes persistent GPU skinning',result['gpu_skinning']['limits'])
            events[3]['skinned_frame_max']=128
            self.assertIn('invalid_gpu_skinning_performance_ramp',review()['errors'])

if __name__=='__main__':unittest.main()
