import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_fg_results import analyze


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


if __name__=='__main__': unittest.main()
