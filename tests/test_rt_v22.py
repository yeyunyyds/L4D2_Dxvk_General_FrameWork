import copy,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import analyze_rt_draw as parser
import test_rt_draw,test_rt_v19
from analyze_rt_live import analyze as live


class V22Evidence(unittest.TestCase):
    def fixture(self):
        helper=test_rt_draw.DrawObservation();original=helper.scene_fixture();events=[(0,0,struct.pack('<IQQQII',19,42,7,10000000,600,30))]
        for i in range(600):
            block=original[1+min(i,239)*4:1+(min(i,239)+1)*4]
            for j in range(2):
                uid=i*4+j+1
                for e,_,p in block[:3]:
                    if e==1:p=bytearray(p);struct.pack_into('<Q',p,16,i*2+j+1);p=bytes(p)
                    events.append((e,uid,p))
            events.append((3,i*4+3,struct.pack('<I',0)))
        events.append((8,0,struct.pack('<6QI',1200,1200,600,0,0,0,0)));return events

    def compact(self,events):
        output=[];templates={}
        for e,u,p in events:
            if e==1:
                n=parser.SNAPSHOT.size+parser.MATERIAL.size;ids=[]
                for kind,raw in [(1,p[40:n]),(2,p[n+32:])]:
                    key=(kind,raw)
                    if key not in templates:
                        ident=len(templates)+1;templates[key]=ident;output.append((9,0,struct.pack('<2I',ident,kind)+raw))
                    ids.append(templates[key])
                output.append((10,u,p[:40]+p[n:n+32]+struct.pack('<2I',*ids)))
            else:output.append((e,u,p))
            if e==3:templates.clear()
        return output

    def test_lossless_template_draws_preserve_every_conversion_and_footer(self):
        helper=test_rt_draw.DrawObservation();raw=self.fixture();encoded=self.compact(raw)
        a=helper.inspect(raw);b=helper.inspect(encoded);self.assertTrue(a['pass'],a['errors']);self.assertTrue(b['pass'],b['errors'])
        for key in ('footer','world_conversion','world_decisions'):self.assertEqual(a[key],b[key])
        self.assertLess(sum(32+len(p) for _,_,p in encoded),sum(32+len(p) for _,_,p in raw))
        damaged=encoded.copy();i=next(i for i,(e,_,_) in enumerate(damaged) if e==10);e,u,p=damaged[i];damaged[i]=(e,u,p[:-4]+struct.pack('<I',4096))
        self.assertIn('missing_draw_template',helper.inspect(damaged)['errors'])

    def test_neutral_materials_and_new_handle_caps_are_version_scoped(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=test_rt_v19.fixture(root)
            for e in events:
                if e['event']=='world_coverage_revision':e['value']='22'
                if e.get('material') in ('metal','alpha','emissive'):e['material']='neutral'
                if e['event']=='model_texture_binding':e['evidence_saved']=True
                if e['event']=='api_mesh_frame':e.update(texture_resident_bytes=0 if e['frame']==600 else e['texture_bytes'],texture_evidence_bytes=e['texture_bytes'],texture_evidence_files=e['texture_exports'],texture_evicted=0,texture_backing_entries=1,texture_backing_hits=0,texture_source_reads=1,texture_invalidated=0)
            result=live(root,events);self.assertTrue(result['pass'],result['errors'])
            next(e for e in events if e['event']=='api_model_path_readback' and e['frame']==480)['material']='alpha'
            self.assertIn('live_readback_sequence',live(root,events)['errors'])

    def test_cleanup_hooks_precede_device_release_and_shutdown_is_idempotent(self):
        root=Path(__file__).resolve().parents[1];patch=(root/'integration/rt/lifecycle-hooks.patch').read_text()
        self.assertLess(patch.index('beforeDeviceDestroy(pD3DDevice)'),patch.index('safeDestroy(pD3DDevice'))
        self.assertLess(patch.index('+  ldb_rt::world_runtime::shutdown();'),patch.index('sampleHostMemory'))
