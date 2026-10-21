from pathlib import Path
import json
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_runtime import analyze
from rt_gpu_telemetry import select_rows,Sampler

class RuntimeEvidenceTests(unittest.TestCase):
    def test_process_and_status_binding_preserves_separate_engines(self):
        rows=[('pid_12_luid_A_eng_0_engtype_3D',0,70.),('pid_12_luid_A_eng_1_engtype_Compute',1,65.),
              ('pid_120_luid_A_eng_0',0,90.),('pid_12_bad',2,99.),('pid_12_nan',0,float('nan')),('pid_12_over',0,101.),('pid_12_negative',0,-1.)]
        result=select_rows(rows,{12})
        self.assertEqual([x['percent'] for x in result],[70.,65.]);self.assertEqual(sum(x['percent'] for x in result),135.)
        self.assertEqual(len(select_rows(rows*200,{12})),128)
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

if __name__=='__main__':unittest.main()
