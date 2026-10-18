import hashlib,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_scene import analyze, verify_model_payloads

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
 def test_v11_model_counts_require_audited_sample_and_final_phase(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root);events[0]['value']='11'
   phases=('raster','baseline_world','expanded_sky','expanded_models')
   models=[dict(event='model_rebuild_counts',frame=f,phase=p,attempts=0,ready=0,reconstructed=0,fallback=0,reasons={}) for f,p in zip((30,120,180,240),phases)]
   events+=models;self.assertTrue(self.inspect(root,events)['pass'])
   models[-1].update(attempts=1,ready=1,reconstructed=1,reasons={'original_vs_model_matrix':1})
   self.assertIn('missing_model_rebuild_samples',self.inspect(root,events)['errors'])
   events.append(dict(event='model_rebuild_sample',phase='expanded_models',ordinal=13,reason='original_vs_model_matrix',rebuilt_stride=32,rebuilt_vertex_bytes=96,applied_world=[float(i%5==0) for i in range(16)]))
   self.assertTrue(self.inspect(root,events)['pass'])
   models[0].update(attempts=1,fallback=1,reasons={'unknown_model_program':1})
   self.assertIn('premature_model_reconstruction',self.inspect(root,events)['errors'])
 def test_v12_model_budget_custom_shader_audit_and_revision_guard(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root);events[0]['value']='12'
   phases=('raster','baseline_world','expanded_sky','expanded_models')
   events += [dict(event='model_rebuild_counts',frame=f,phase=p,attempts=0,ready=0,reconstructed=0,fallback=0,reasons={}) for f,p in zip((30,120,180,240),phases)]
   sample=events[1];sample['vertex_count']=8952;sample['args']=[0,2984,0,0,0,0]
   raw=struct.pack('<3f',1,2,-10)*8952;path=root/'scene-12-vertices.bin';path.write_bytes(raw);sample['files'][0].update(size=len(raw),sha256=hashlib.sha256(raw).hexdigest())
   self.assertTrue(self.inspect(root,events)['pass'])
   events[0]['value']='11';self.assertFalse(self.inspect(root,events)['pass'])
   events[0]['value']='12';sample['shader']='NekoToon';self.assertTrue(self.inspect(root,events)['pass'])
   sample['shader']='UnlitGeneric';self.assertFalse(self.inspect(root,events)['pass'])
   sample['shader']='NekoToon'
   sample['vertex_count']=3;sample['args']=[0,1,0,0,0,0];raw=raw[:36];path.write_bytes(raw);sample['files'][0].update(size=len(raw),sha256=hashlib.sha256(raw).hexdigest())
   self.assertTrue(self.inspect(root,events)['pass'])
 def test_wide_window_and_omission_accounting(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root);events[0]['value']='12';events.insert(1,dict(event='scene_input_policy',code=0,value='wide_materials_v1'));sample=events[2]
   sample.update(kind=2,index_bytes=2,args=[0,0,32021,0,1,0],first_absolute_vertex=32018,geometry_window=True,source_vertex_buffer_bytes=32021*12,stream_offset=0)
   raw=struct.pack('<3H',32018,32019,32020);name='scene-12-indices.bin';(root/name).write_bytes(raw);sample['files'].append(dict(file=name,size=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
   for frame,phase in zip((30,120,180,240),('raster','baseline_world','expanded_sky','expanded_models')):
    events.append(dict(event='model_rebuild_counts',frame=frame,phase=phase,attempts=0,ready=0,reconstructed=0,fallback=0,reasons={}))
    events.append(dict(event='scene_input_counts',frame=frame,phase=phase,payload_bytes=sum(x['size'] for x in sample['files']) if frame==240 else 0,captured=int(frame==240),identity_failures=0,family_payload_bytes=[0,sum(x['size'] for x in sample['files']) if frame==240 else 0,0,0],family_captured=[0,int(frame==240),0,0],family_quota_omitted=[0,0,0,3 if frame==240 else 0],family_payload_omitted=[0,1 if frame==240 else 0,0,0]))
   self.assertTrue(self.inspect(root,events)['pass'])
   sample['first_absolute_vertex']-=1;self.assertFalse(self.inspect(root,events)['pass']);sample['first_absolute_vertex']+=1
   sample['source_vertex_buffer_bytes']-=1;self.assertFalse(self.inspect(root,events)['pass']);sample['source_vertex_buffer_bytes']+=1
   events[-1]['payload_bytes']+=1;self.assertIn('inconsistent_wide_capture_counts',self.inspect(root,events)['errors'])
 def test_v13_complete_morph_and_full_pixel_constants_are_required(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=self.fixture(root);events[0]['value']='13';sample=events[1]
   phases=('raster','baseline_world','expanded_sky','expanded_models')
   events += [dict(event='model_rebuild_counts',frame=f,phase=p,attempts=0,ready=0,reconstructed=0,fallback=0,reasons={}) for f,p in zip((30,120,180,240),phases)]
   sample['shader']='NekoToon';sample['pixel_constants_hresult']=0
   def payload(suffix,data):
    name='scene-12-'+suffix;(root/name).write_bytes(data)
    sample['files'].append(dict(file=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
   payload('ps-constants.bin',bytes(224*16));payload('morph-positions.bin',struct.pack('<4f',1,2,3,0)*3)
   sample['morph_position']=dict(hresult=0,stream=2,offset=16,stride=28,frequency=1,source_buffer_bytes=100,first_absolute_vertex=0,vertices=3)
   self.assertTrue(self.inspect(root,events)['pass'])
   sample['morph_position']['vertices']=2;self.assertIn('invalid_complete_morph_position',self.inspect(root,events)['errors']);sample['morph_position']['vertices']=3
   sample['morph_position']['source_buffer_bytes']=87;self.assertIn('morph_position_outside_source_buffer',self.inspect(root,events)['errors']);sample['morph_position']['source_buffer_bytes']=100
   sample['morph_position']['frequency']=2;self.assertFalse(self.inspect(root,events)['pass']);sample['morph_position']['frequency']=1
   saved=sample.pop('morph_position');self.assertIn('missing_morph_position_metadata',self.inspect(root,events)['errors']);sample['morph_position']=saved
   sample['files'][-2]['size']=512
   self.assertFalse(self.inspect(root,events)['pass'])
 def test_old_capture_does_not_require_new_samples(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);self.assertEqual(self.inspect(root,[dict(event='world_coverage_revision',code=0,value='8')])['status'],'not_requested')

 def test_v14_compact_submission_hash_indices_and_binding_proof(self):
  with tempfile.TemporaryDirectory() as temporary:
   root=Path(temporary);raw={'vertices.bin':struct.pack('<8f',1,2,3,.2,.3,0,0,0)*3,'indices.bin':struct.pack('<3H',0,1,2),'source-vertices.bin':bytes(96),'source-indices.bin':struct.pack('<3H',3,4,5),'vs.bin':struct.pack('<2I',0xfffe0300,0xffff),'ps.bin':struct.pack('<2I',0xffff0300,0xffff),'constants.bin':bytes(4096),'declaration.bin':bytes.fromhex('000000000200000000000c0001000500ff00000011000000')}
   sample=dict(ordinal=10,frame=240,model_stage='ordinary',upload_verified=True,binding_verified=True,owned_draw=True,payload_omitted=False,rebuilt_vertex_bytes=96,index_bytes=2,draw_args=[0,0,3,0,1,0],kind=2,topology=4,original_args=[2,3,3,7,1,0],source_first=5,source_count=3,source_stride=32,files=[])
   for suffix,data in raw.items():
    name='model-10-'+suffix;(root/name).write_bytes(data);sample['files'].append(dict(file=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
   errors=[];self.assertEqual(verify_model_payloads(root,[sample],errors),sum(map(len,raw.values())));self.assertEqual(errors,[])
   newer=dict(sample,frame=240,model_stage='neko',preserve_requested=False,neutral_material=False,vertices_sha256=hashlib.sha256(raw['vertices.bin']).hexdigest(),indices_sha256=hashlib.sha256(raw['indices.bin']).hexdigest(),material_query_failure_bits=0,material_stage0=[2,2,2,2],material_texture_factor=0xffffffff)
   errors=[];verify_model_payloads(root,[newer],errors,revision=15);self.assertEqual(errors,[])
   newer['vertices_sha256']='0'*64;errors=[];verify_model_payloads(root,[newer],errors,revision=15);self.assertIn('model_submission_payload_digest_mismatch',errors)
   newer.update(model_stage='combined_matte',frame=480,payload_omitted=True,files=[],neutral_material=True,material_stage0=[2,3,2,2],material_texture_factor=0xff808080)
   errors=[];verify_model_payloads(root,[newer],errors,revision=15);self.assertEqual(errors,[])
   newer['material_stage0']=[2,2,2,2];errors=[];verify_model_payloads(root,[newer],errors,revision=15);self.assertIn('model_material_binding_mismatch',errors)
   sample['binding_verified']=False;errors=[];verify_model_payloads(root,[sample],errors);self.assertIn('unverified_model_submission',errors)
   sample['binding_verified']=True;bad=struct.pack('<3H',0,1,3);(root/'model-10-indices.bin').write_bytes(bad);sample['files'][1]['sha256']=hashlib.sha256(bad).hexdigest()
   errors=[];verify_model_payloads(root,[sample],errors);self.assertIn('compact_model_index_outside_vertices',errors)
   sample['payload_omitted']=True;errors=[];verify_model_payloads(root,[sample],errors);self.assertIn('omitted_model_payload_present',errors)

 def test_v14_stage_summaries_require_recovery_and_keep_main_phase_budget(self):
  with tempfile.TemporaryDirectory() as temporary:
   root=Path(temporary);events=self.fixture(root);events[0]['value']='14';events[1]['morph_position']={'hresult':1,'vertices':0}
   for e in events:
    if e.get('event')=='sky_rebuild_counts' and e['frame']==240:e['frame']=420
   phases=('raster','baseline_world','expanded_sky')+('expanded_models',)*4
   events.extend(dict(event='model_rebuild_counts',frame=f,phase=p,attempts=0,ready=0,reconstructed=0,fallback=0,reasons={}) for f,p in zip((30,120,180,240,300,360,420),phases))
   report=self.inspect(root,events);self.assertTrue(report['pass'],report['errors'])
   events.pop();self.assertIn('missing_model_rebuild_summaries',self.inspect(root,events)['errors'])

 def test_v15_stage_summaries_require_complete_600_and_keep_source_budget(self):
  with tempfile.TemporaryDirectory() as temporary:
   root=Path(temporary);events=self.fixture(root);events[0]['value']='15';events[1]['morph_position']={'hresult':1,'vertices':0}
   for e in events:
    if e.get('event')=='sky_rebuild_counts' and e['frame']==240:e['frame']=600
   phases=('raster','baseline_world','expanded_sky')+('expanded_models',)*7
   events.extend(dict(event='model_rebuild_counts',frame=f,phase=p,attempts=0,ready=0,reconstructed=0,fallback=0,reasons={}) for f,p in zip((30,120,180,240,300,360,420,480,540,600),phases))
   report=self.inspect(root,events);self.assertTrue(report['pass'],report['errors'])
   events.pop();self.assertIn('missing_model_rebuild_summaries',self.inspect(root,events)['errors'])

if __name__=='__main__':unittest.main()
