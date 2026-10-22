import hashlib,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_infected import analyze


class V21Evidence(unittest.TestCase):
    def fixture(self,root):
        events=[dict(event='world_coverage_revision',value='21',code=0)]
        vertices=struct.pack('<8f',1,2,3,.2,.3,0,0,1)*3;indices=struct.pack('<3H',0,1,2)
        row=dict(event='infected_near_target',ordinal=42,frame=300,preview=False,ready=True,submitted=True,reason='original_vs_infected_rigid48',distance_source_units=64,center_clip=[0,0,.5,1],world_center=[1,2,3],payload_bytes=len(vertices)+len(indices))
        for key,raw in [('vertices',vertices),('indices',indices)]:
            name=f'target-42-{key}.bin';(root/name).write_bytes(raw);row['vertex_file' if key=='vertices' else 'index_file']=name;row[key+'_sha256']=hashlib.sha256(raw).hexdigest()
        events.append(row);events.append(dict(event='live_shader_summary',near_target_rows=1,near_target_payload_bytes=len(vertices)+len(indices),infected_bakes=1))
        h=[0]*37;h[0]=0x20534444;h[1]=124;h[3]=h[4]=h[7]=1;h[32:37]=[87,3,0,1,0]
        raw=struct.pack('<37I',*h)+bytes([50,100,150,255]);digest=hashlib.sha256(raw).hexdigest();name=f'model-textures/{digest}.dds';(root/'model-textures').mkdir();(root/name).write_bytes(raw)
        recipe=dict(event='infected_diffuse_bake',frame=241,material=4,output_sha256=digest,output_file=name,output_evidence_saved=True,parameters=[.1,.9,0,0]+[1,1,1,0]+[0]*12,inputs=[dict(slot=slot,sha256=digest,file=name,evidence_saved=True,srgb=False,wrap_u=1,wrap_v=1,filter=1) for slot in [0,4,5]])
        def save():
            (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events));(root/'infected-materials.jsonl').write_text(json.dumps(recipe)+'\n')
        save();return events,recipe,save

    def test_target_payload_and_recipe_integrity(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events,recipe,save=self.fixture(root);result=analyze(root)
            self.assertTrue(result['pass'],result);self.assertEqual(result['near_submitted_rows'],1);self.assertEqual(result['gpu_visibility'],'unrun')
            (root/events[1]['vertex_file']).write_bytes(b'corrupt');self.assertIn('infected_target_digest',analyze(root)['errors'])

    def test_sampler_graph_opaque_output_and_final_accounting(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events,recipe,save=self.fixture(root)
            recipe['inputs'][1]['slot']=3;save();self.assertIn('infected_recipe_samplers',analyze(root)['errors']);recipe['inputs'][1]['slot']=4
            events[-1]['near_target_rows']=0;save();self.assertIn('infected_final_accounting',analyze(root)['errors']);events[-1]['near_target_rows']=1
            recipe['output_evidence_saved']=False
            for s in recipe['inputs']:s['evidence_saved']=False
            save();(root/recipe['output_file']).unlink();result=analyze(root);self.assertTrue(result['pass'],result);self.assertEqual(result['omitted_dependency_files'],1)

    def test_older_capture_remains_explicitly_not_requested(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'world-runtime.jsonl').write_text('{"event":"world_coverage_revision","value":"20","code":0}')
            self.assertEqual(analyze(root)['status'],'not_requested')
    def test_v25_skin_payload_layout_and_accounting(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events,recipe,save=self.fixture(root);events.insert(1,dict(event='pilot_delivery_revision',value='25'));row=events[2]
            payloads={'skin_weights':struct.pack('<9f',*([.25,.5,.25]*3)),
                      'skin_indices':struct.pack('<9I',*([0,1,2]*3)),
                      'bone_transforms':struct.pack('<36f',*([1,0,0,0,0,1,0,0,0,0,1,0]*3))}
            for key,raw in payloads.items():
                suffix=key.replace('_','-');name=f'target-42-{suffix}.bin';(root/name).write_bytes(raw)
                row[{'skin_weights':'skin_weight_file','skin_indices':'skin_index_file','bone_transforms':'bone_transform_file'}[key]]=name
                row[key+'_sha256']=hashlib.sha256(raw).hexdigest();row['payload_bytes']+=len(raw);events[-1]['near_target_payload_bytes']+=len(raw)
            row['skinned']=True;save();result=analyze(root);self.assertTrue(result['pass'],result)
            row['skin_indices_sha256']='0'*64;save();self.assertIn('target_skin_payload',analyze(root)['errors'])
    def test_v28_canary_retains_skin_payload_validation(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events,recipe,save=self.fixture(root);events.insert(1,dict(event='pilot_delivery_revision',value='28'));row=events[2]
            empty=hashlib.sha256(b'').hexdigest();row.update(skinned=False,skin_weight_file='',skin_index_file='',bone_transform_file='',skin_weights_sha256=empty,skin_indices_sha256=empty,bone_transforms_sha256=empty)
            save();self.assertTrue(analyze(root)['pass'])
            row['skinned']=True;save();self.assertIn('target_skin_path',analyze(root)['errors'])
