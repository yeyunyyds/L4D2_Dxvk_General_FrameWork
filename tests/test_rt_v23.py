import copy,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import test_rt_v19,test_rt_v22,test_rt_draw
from analyze_rt_live import analyze

class V23DisplayEvidence(unittest.TestCase):
    def test_trace20_keeps_all_draws_and_marks_revision23(self):
        h=test_rt_v22.V22Evidence();events=h.compact(h.fixture());e,u,p=events[0];events[0]=(e,u,struct.pack('<I',20)+p[4:])
        result=test_rt_draw.DrawObservation().inspect(events)
        self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['header']['coverage_revision'],23)
        self.assertEqual(result['footer']['draws'],1200)

    def test_new_runtime_capacity_and_geometry_limits_are_revision_scoped(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=test_rt_v19.fixture(root)
            for e in events:
                if e['event']=='world_coverage_revision':e['value']='23'
                if e.get('material') in ('metal','alpha','emissive'):e['material']='neutral'
                if e['event']=='model_texture_binding':e.update(evidence_saved=True,alpha_blend=False,source_material=1,source_shader='VertexLitGeneric')
                if e['event']=='api_mesh_frame':
                    e.update(texture_resident_bytes=0 if e['frame']==600 else e['texture_bytes'],texture_evidence_bytes=e['texture_bytes'],texture_evidence_files=e['texture_exports'],texture_evicted=0,texture_backing_entries=1,texture_backing_hits=0,texture_source_reads=1,texture_invalidated=0,texture_rejected_entries=0,texture_retry_suppressed=0)
                    if e['frame']>=300:e.update(texture_bytes=800*1024*1024,peak_bytes=300*1024*1024)
                    if e['frame']==300:e.update(bytes=300*1024*1024,frame_new_bytes=150*1024*1024)
            self.assertTrue(analyze(root,events)['pass'],analyze(root,events)['errors'])
            legacy=copy.deepcopy(events)
            for e in legacy:
                if e['event']=='world_coverage_revision':e['value']='22'
            self.assertFalse(analyze(root,legacy)['pass'])
            next(e for e in events if e['event']=='api_mesh_frame' and e['frame']==600)['texture_bytes']=4294967297
            self.assertIn('texture_runtime_budget',analyze(root,events)['errors'])
