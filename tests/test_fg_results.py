import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_fg_results import analyze, analyze_layer


class SessionResultsTests(unittest.TestCase):
    def analyze(self,events):
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp)/'fg-events.jsonl'
            p.write_text(''.join(json.dumps(e)+'\n' for e in events))
            return analyze(p)

    def test_preview_is_not_an_extra_present(self):
        result=self.analyze([
            dict(event='generated_present',mode='paired',hresult=0),
            dict(event='generated_present',mode='generated',hresult=0),
            dict(event='summary',logical_presents=10)])
        self.assertEqual(result['backend_calls'],11)
        self.assertEqual(result['successful_generated_presents'],2)

    def test_incomplete_session_does_not_invent_totals(self):
        result=self.analyze([dict(event='generated_present',mode='paired',hresult=0x8876086c)])
        self.assertFalse(result['log_complete'])
        self.assertIsNone(result['backend_calls'])
        self.assertEqual(result['successful_generated_presents'],0)

    def test_rejected_candidate_is_not_a_generated_present(self):
        rejection=dict(event='frame_rejected',frame=3,reason='invalid_scene_coverage',
                       invalid_scene_ratio=.38,original_restored=True,presented=False)
        result=self.analyze([rejection,dict(event='summary',logical_presents=10)])
        self.assertEqual(result['rejected_candidates'],[rejection])
        self.assertEqual(result['rejected_candidate_count'],1)
        self.assertEqual(result['backend_calls'],10)
        self.assertEqual(result['successful_generated_presents'],0)

    def test_capture_stall_excluded_from_arrival_distribution(self):
        events=[dict(event='session',qpc_frequency=1000)]
        # Interval after captured frame includes readback stalls and must not enter statistics.
        for f,t,captured in [(0,100,False),(1,110,True),(2,300,False),(3,310,False)]:
            events.append(dict(event='frame_metrics',frame=f,generation=0,arrival_qpc=t,
                               enabled=True,generated=True,mode='paired',captured=captured,
                               host_work_ms=2,gpu_dispatch_ms=-1,fence_wait_ms=1,gpu_submissions=6,
                               generated_present_ms=.1,logical_present_ms=.1,pacing_wait_ms=0,
                               paired_start_gap_ms=1))
        result=self.analyze(events)['metrics']['paired']
        self.assertEqual(result['source_arrival_interval_ms']['samples'],1)
        self.assertEqual(result['source_arrival_interval_ms']['median'],10)
        self.assertIsNone(result.get('gpu_dispatch_ms'))

    def test_existing_upload_without_metrics_remains_readable(self):
        result=self.analyze([dict(event='session',qpc_frequency=1000),dict(event='summary',logical_presents=22)])
        self.assertTrue(result['log_complete'])
        self.assertEqual(result['metrics'],{})

    def test_native_and_disabled_windows_report_ab_input_rates_without_claiming_scanout(self):
        events=[dict(event='session',qpc_frequency=1000)]
        for frame,arrival,enabled,native in [
                (0,0,False,0),(1,20,False,0),(2,40,False,0),
                (3,100,True,1),(4,110,True,2),(5,120,True,3)]:
            events.append(dict(event='frame_metrics',frame=frame,generation=0,
                arrival_qpc=arrival,enabled=enabled,generated=enabled,
                mode='host_provider',captured=False,native_accepted_total=native,
                host_work_ms=1,gpu_dispatch_ms=-1,fence_wait_ms=0,
                gpu_submissions=1,generated_present_ms=0,logical_present_ms=0,
                pacing_wait_ms=0,paired_start_gap_ms=0))
        result=self.analyze(events)['ab_comparison']
        self.assertEqual(result['disabled_input_fps_median'],50)
        self.assertEqual(result['native_input_fps_median'],100)
        self.assertEqual(result['expected_generated_output_fps'],200)
        self.assertTrue(result['expected_output_is_not_physical_scanout'])

    def test_presenter_device_topology_is_preserved(self):
        result=self.analyze([dict(event='session',qpc_frequency=1000,
            presenter_requested=True,presenter_device_api=True,
            presenter_owner_match=True,presenter_device_flags=7,
            presenter_feature_flags=15,presenter_graphics_family=2,
            presenter_graphics_queues=4,presenter_reserved_queues=3,
            presenter_topology_api=True,
            presenter_queue_families=[dict(family=0,flags=7,physical=1,created=1)],
            presenter_input_api=1),
            dict(event='presenter_input',valid=True,flags=151,
                 gpu_ready_token=False,present_ticket=False,
                 motion_source='depth_camera_reconstruction'),
            dict(event='summary',logical_presents=1,generated_presents=0,
                 presenter_inputs_accepted=1,presenter_inputs_rejected=0)])
        self.assertEqual(result['presenter_device'],dict(
            requested=True,device_api=True,owner_match=True,device_flags=7,
            feature_flags=15,graphics_family=2,graphics_queues=4,
            reserved_queues=3,topology_api=True,
            queue_families=[dict(family=0,flags=7,physical=1,created=1)],
            input_api=1))
        self.assertEqual(result['presenter_inputs'],dict(
            packets=1,valid=1,gpu_ready=0,present_ticket=0,
            rejected_events=0,motion_source='depth_camera_reconstruction',
            accepted_total=1,rejected_total=0))

    def test_layer_acceptance_separates_dispatch_from_content_proof(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'asteria-layer.json'
            path.write_text(json.dumps({
                'host_frame_api_version':2,'host_frame_submitted':121,
                'host_frame_accepted':120,'host_frame_consumed':119,
                'host_frame_rejected':1,'host_frame_stale_at_present':0,
                'host_scene_depth_semantic_observed':True,
                'host_scene_depth_image_consumed':True,
                'native_v2_ticket_queries_accepted':120,
                'native_v2_submitted':120,'native_v2_accepted':119,
                'native_v2_leases_released':118,
                'native_present_successes':240,'native_present_failures':0,
                'swapchains':{'1':{'live':True,'frame_input_attempts':120,
                    'interpolation_dispatches':119,'present_count_ffx':239,
                    'native_frame_inputs_bound':True,
                    'native_interpolation_dispatches':119,
                    'host_camera_control_active':True,
                    'host_depth_resource_connected':True,
                    'generated_content_distinctness':'unverified_no_gpu_content_probe',
                    'motion_input_source':1}}}))
            result=analyze_layer(path)
        self.assertTrue(result['host_control']['active'])
        self.assertTrue(result['host_control']['gpu_inputs_active'])
        self.assertTrue(result['frame_generation']['algorithm_dispatch_confirmed'])
        self.assertTrue(result['frame_generation']['native_algorithm_dispatch_confirmed'])
        self.assertEqual(result['acceptance_level'],
                         'native_v2_ticketed_swapchain_color_algorithm_and_extra_present_confirmed')
        self.assertTrue(result['frame_generation']['extra_present_submission_confirmed'])
        self.assertEqual(result['frame_generation']['generated_content_distinctness'],
                         'unverified_no_gpu_content_probe')
        self.assertFalse(result['frame_generation']['physical_scanout_confirmed'])


if __name__=='__main__': unittest.main()
