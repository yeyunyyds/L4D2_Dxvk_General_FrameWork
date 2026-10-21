import copy,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import test_rt_v19,test_rt_v22,test_rt_draw
from analyze_rt_live import analyze

class V23DisplayEvidence(unittest.TestCase):
    def trace_events(self):
        h=test_rt_v22.V22Evidence();events=h.fixture();e,u,p=events[0];events[0]=(e,u,struct.pack('<I',20)+p[4:])
        for i,(kind,uid,raw) in enumerate(events):
            if kind!=1:continue
            at=test_rt_draw.check.SNAPSHOT.size+test_rt_draw.check.MATERIAL.size+32+test_rt_draw.check.PIPELINE_V8.size
            events[i]=(kind,uid,raw[:at]+struct.pack('<8I',5,6,1,0,0,0,15,4)+raw[at:])
        return h,events

    def test_trace20_keeps_all_draws_and_marks_revision23(self):
        h,events=self.trace_events();result=test_rt_draw.DrawObservation().inspect(h.compact(events))
        self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['header']['coverage_revision'],23)
        self.assertEqual(result['footer']['draws'],1200)

    def test_alpha_conversion_requires_complete_blend_and_depth_proof(self):
        h,events=self.trace_events();i=next(i for i,(kind,uid,raw) in enumerate(events) if kind==1 and uid>=241*4+1)
        kind,uid,raw=events[i];source=next(p for k,u,p in events if k==1 and test_rt_draw.check.MATERIAL.unpack_from(p,test_rt_draw.check.SNAPSHOT.size)[3].startswith(b'VertexLitGeneric'));data=bytearray(source);struct.pack_into('<Q',data,16,struct.unpack_from('<Q',raw,16)[0]);at=test_rt_draw.check.SNAPSHOT.size+test_rt_draw.check.MATERIAL.size+32
        struct.pack_into('<I',data,at+4*4,1);struct.pack_into('<I',data,at+7*4,0)
        events[i]=(kind,uid,bytes(data));events[i+1]=(6,uid,struct.pack('<8I',12,9,0,0,0,1,1,0))
        result=test_rt_draw.DrawObservation().inspect(h.compact(events));self.assertTrue(result['pass'],result['errors'])
        for field,value in [(22,2),(23,2),(24,2),(25,1),(26,1),(27,1),(28,8),(29,8)]:
            bad=events.copy();changed=data.copy();struct.pack_into('<I',changed,at+field*4,value);bad[i]=(kind,uid,bytes(changed))
            self.assertIn('unsafe_world_conversion_or_control_modified',test_rt_draw.DrawObservation().inspect(h.compact(bad))['errors'])

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
