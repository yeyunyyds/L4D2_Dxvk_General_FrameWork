import copy,json,struct,sys,tempfile,unittest,zipfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import test_rt_v19
from analyze_rt_live import analyze
from rt_draw_suite import review_preview,archive_result

class V20Evidence(unittest.TestCase):
    def test_working_and_evidence_limits_are_separate(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=test_rt_v19.fixture(root)
            for e in events:
                if e['event']=='world_coverage_revision':e['value']='20'
                if e['event']=='model_texture_binding':e['evidence_saved']=True
                if e['event']=='api_mesh_frame':
                    e.update(texture_resident_bytes=0 if e['frame']==600 else e['texture_bytes'],texture_evidence_bytes=e['texture_bytes'],texture_evidence_files=e['texture_exports'],texture_evicted=0)
            result=analyze(root,events);self.assertTrue(result['pass'],result['errors'])
            missing=copy.deepcopy(events)
            for e in missing:
                if e['event']=='model_texture_binding':e['evidence_saved']=False
            result=analyze(root,missing);self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['textures']['evidence_omitted_files'],1);self.assertEqual(result['textures']['unique_dds_files'],0)
            next(e for e in events if e['event']=='api_mesh_frame' and e['frame']==599)['texture_resident_bytes']=128*1024*1024+1
            self.assertIn('texture_working_evidence_budget',analyze(root,events)['errors'])

    def test_preview_is_separate_and_requires_duration_images_and_retirement(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=[]
            raw=struct.pack('<4f',.2,.3,.4,1)*(640*400)
            for i in range(3):
                name=f'preview-{i}.rgba32f';(root/name).write_bytes(raw)
                events.append(dict(event='preview_readback',file=name,copy_code=0,hresult=0,written=True))
            events.append(dict(event='preview_end',complete=True,elapsed_ms=60000,submitted=10,entries=0,pending_fences=0,held=0))
            def review():
                (root/'preview-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return review_preview(root)
            self.assertTrue(review()['pass']);events[-1]['complete']=False;self.assertFalse(review()['pass']);events[-1]['complete']=True
            events[-1]['entries']=1;self.assertIn('preview_duration_or_completion',review()['errors']);events[-1]['entries']=0
            (root/'preview-1.rgba32f').write_bytes(struct.pack('<f',float('nan'))+raw[4:]);self.assertIn('preview_nonfinite_rgb',review()['errors'])

    def test_runtime_backing_is_excluded_without_silent_archive_loss(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'result';(root/'runtime-textures').mkdir(parents=True);(root/'model-textures').mkdir()
            (root/'runtime-textures/secret.dds').write_bytes(b'runtime');(root/'model-textures/evidence.dds').write_bytes(b'evidence')
            report=dict(collection='complete',errors=[]);output=archive_result(root,report)
            with zipfile.ZipFile(output) as z:
                self.assertNotIn('runtime-textures/secret.dds',z.namelist());self.assertEqual(z.read('model-textures/evidence.dds'),b'evidence');self.assertEqual(json.loads(z.read('omitted-files.json')),[])
            self.assertEqual(report['collection'],'complete')
