import hashlib,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_scene import analyze

class SceneEvidence(unittest.TestCase):
 def fixture(self,root):
  m=[float(i%5==0) for i in range(16)];decl=bytes.fromhex('0000000002000000ff00000011000000')
  files={'vertices.bin':struct.pack('<3f',1,2,-10)*3,'declaration.bin':decl,'constants.bin':bytes(4096)}
  sample=dict(event='scene_input_sample',phase='expanded_models',ordinal=12,material=1,shader='VertexLitGeneric',format=0x80503,geometry_status='complete',replay_status='not_sky',geometry_hresult=0,shader_hresult=0x80004005,constants_hresult=0,first_absolute_vertex=0,vertex_count=3,stride=12,kind=1,args=[0,1,0,0,0,0],topology=4,world=m,view=m,projection=m,files=[])
  for suffix,raw in files.items():
   name='scene-12-'+suffix;(root/name).write_bytes(raw);sample['files'].append(dict(file=name,size=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
  events=[dict(event='world_coverage_revision',code=0,value='9'),sample]
  for frame,phase in zip((30,120,180,240),('raster','baseline_world','expanded_sky','expanded_models')):events.append(dict(event='sky_rebuild_counts',frame=frame,phase=phase,attempts=0,ready=0,reconstructed=0,fallback=0,input_samples=int(frame==240),reasons={}))
  return events
 def inspect(self,root,events):
  (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return analyze(root)
 def test_bounded_files_and_raw_projection_do_not_claim_model_visibility(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);r=self.inspect(root,self.fixture(root));self.assertTrue(r['pass'],r['errors']);self.assertEqual(r['samples'][0]['raw_geometry']['front_vertices'],3);self.assertEqual(r['sky_rebuild_result'],'partial_or_unrun')
 def test_corrupt_files_unsafe_paths_and_false_vertex_ranges_reject(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root)
   events[1]['files'][0]['sha256']='0'*64;self.assertFalse(self.inspect(root,events)['pass'])
   events=self.fixture(root);events[1]['files'][0]['file']='../scene-12-vertices.bin';self.assertFalse(self.inspect(root,events)['pass'])
   events=self.fixture(root);events[1]['vertex_count']=4097;self.assertFalse(self.inspect(root,events)['pass'])
 def test_fallback_is_explicit_and_counter_mismatch_rejects(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root);events[-1].update(attempts=360,fallback=360,reasons={'unsupported_opcode':360});r=self.inspect(root,events);self.assertTrue(r['pass']);self.assertEqual(r['sky_rebuild_result'],'fallback_preserved_v8')
   events[-1]['ready']=1;events[-1]['reconstructed']=1;self.assertFalse(self.inspect(root,events)['pass'])
 def test_v10_wider_geometry_and_balanced_quota_validation(self):
  import copy
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root);events[0]['value']='10';sample=events[1]
   raw=struct.pack('<3f',1,2,-10)+bytes(68);raw*=3
   path=root/'scene-12-vertices.bin';path.write_bytes(raw);sample['files'][0].update(size=len(raw),sha256=hashlib.sha256(raw).hexdigest());sample['stride']=80
   self.assertTrue(self.inspect(root,events)['pass'])
   for ordinal in range(13,21):
    other=copy.deepcopy(sample);other['ordinal']=ordinal
    for item in other['files']:
     source=root/item['file'];item['file']=item['file'].replace('scene-12-','scene-'+str(ordinal)+'-');(root/item['file']).write_bytes(source.read_bytes())
    events.insert(2,other)
   events[-1]['input_samples']=9
   r=self.inspect(root,events);self.assertIn('scene_family_quota_exceeded',r['errors'])
 def test_old_capture_does_not_require_new_samples(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);self.assertEqual(self.inspect(root,[dict(event='world_coverage_revision',code=0,value='8')])['status'],'not_requested')
if __name__=='__main__':unittest.main()
