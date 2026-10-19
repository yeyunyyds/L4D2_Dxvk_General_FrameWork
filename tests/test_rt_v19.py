import copy,hashlib,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import test_rt_live as live_cases
import test_rt_scene as scene_cases
from analyze_rt_live import analyze
from analyze_rt_scene import analyze as analyze_scene
from rt_draw_suite import review_exit_logs


def fixture(root):
    events=live_cases.LiveEvidence().v18_fixture(root)
    first=next(e for e in events if e['event']=='model_texture_binding')
    raw=bytearray((root/first['file']).read_bytes());raw.extend(bytes(16))
    struct.pack_into('<I',raw,8,0xa1007);struct.pack_into('<I',raw,28,3);struct.pack_into('<I',raw,108,0x401008)
    digest=hashlib.sha256(raw).hexdigest();name='model-textures/'+digest+'.dds';(root/name).write_bytes(raw)
    for e in events:
        if e['event']=='world_coverage_revision':e['value']='19'
        if e['event']=='model_texture_binding':e.update(file=name,sha256=digest,mip_levels=3,full_mip_levels=3,reason='sampler0_source_mips_dds_dx10')
        if e['event']=='api_mesh_frame':e['texture_bytes']=len(raw)*int(e['frame']>180)
        if e['event']=='model_light_config_requested':e['value']='3,3,3' if e['after_present']==300 else '30,30,30'
    events.append(dict(event='model_light_config_requested',after_present=360,value='30,30,30',code=0))
    # Runtime events are ordered; fixture originally appends controls at end.
    lights=sorted([e for e in events if e['event']=='model_light_config_requested'],key=lambda e:e['after_present'])
    events=[e for e in events if e['event']!='model_light_config_requested']+lights
    return events


class V19Evidence(unittest.TestCase):
    def test_full_mips_light_sequence_and_truncated_tail(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=fixture(root);result=analyze(root,events)
            self.assertTrue(result['pass'],result['errors']);self.assertEqual(result['textures']['complete_mip_files'],1)
            binding=next(e for e in events if e['event']=='model_texture_binding');raw=(root/binding['file']).read_bytes()[:-1]
            digest=hashlib.sha256(raw).hexdigest();name='model-textures/'+digest+'.dds';(root/name).write_bytes(raw)
            for e in events:
                if e['event']=='model_texture_binding':e.update(file=name,sha256=digest)
            self.assertIn('texture_payload_size',analyze(root,events)['errors'])

    def test_wrong_mip_metadata_light_and_budget_fail(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=fixture(root);wrong=copy.deepcopy(events)
            next(e for e in wrong if e['event']=='model_texture_binding')['mip_levels']=1
            self.assertIn('texture_mip_metadata',analyze(root,wrong)['errors'])
            wrong=[e for e in events if not(e['event']=='model_light_config_requested' and e['after_present']==360)]
            self.assertIn('texture_light_sequence',analyze(root,wrong)['errors'])
            next(e for e in events if e['event']=='api_mesh_frame' and e['frame']==599)['texture_bytes']=128*1024*1024+1
            self.assertIn('texture_runtime_budget',analyze(root,events)['errors'])

    def test_large_infected_input_is_audit_only(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);events=scene_cases.SceneEvidence().fixture(root);events[0]['value']='19';sample=events[1]
            sample.update(shader='Infected',vertex_count=6000,args=[0,2000,0,0,0,0],morph_position={},geometry_window=True,source_vertex_buffer_bytes=72000,stream_offset=0)
            raw=struct.pack('<3f',1,2,-10)*6000;path=root/'scene-12-vertices.bin';path.write_bytes(raw)
            sample['files'][0].update(size=len(raw),sha256=hashlib.sha256(raw).hexdigest())
            events.append(dict(event='scene_input_policy',code=0,value='skinned_materials_v2'))
            (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            result=analyze_scene(root)
            self.assertEqual(result['samples'][0]['shader'],'Infected');self.assertEqual(result['samples'][0]['raw_geometry']['front_vertices'],6000)
            self.assertNotEqual(result.get('model_rebuild_result'),'complete')

    def test_exit_fault_is_separate_from_cleanup_and_pid_bound(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);folder=root/'game-logs/0';folder.mkdir(parents=True)
            block='===== L4D2_BRIDGE_EXCEPTION_BEGIN =====\nside=client pid=123\nexception_code=0xc0000005\ntarget_address=0x104b0\nframe1 address=0x1234 module=necola.dll module_offset=0x1447fc\n===== L4D2_BRIDGE_EXCEPTION_END ====='
            entries=[]
            for file,text in [('bridge32.log','Shutdown cleanup successful\n'+block),('bridge64.log','Shutdown cleanup successful')]:
                path=folder/file;path.write_text(text);entries.append(dict(file=str(path.relative_to(root)),sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
            (root/'game-log-inventory.json').write_text(json.dumps(dict(copied=entries)))
            (root/'remix-dxvk.log').write_text('[40] common device objects were not disposed of.')
            result=review_exit_logs(root,123);self.assertEqual(result['status'],'abnormal');self.assertEqual(result['backend_objects_remaining'],40)
            self.assertTrue(result['exceptions'][0]['pid_bound']);self.assertEqual(result['exceptions'][0]['named_frames'][0]['module'],'necola.dll')
            self.assertEqual(review_exit_logs(root,999)['status'],'unverified')
            (folder/'bridge32.log').write_text('Shutdown cleanup successful');entries[0]['sha256']=hashlib.sha256((folder/'bridge32.log').read_bytes()).hexdigest()
            (root/'game-log-inventory.json').write_text(json.dumps(dict(copied=entries)))
            self.assertEqual(review_exit_logs(root,123)['status'],'cleanup_warning');(root/'remix-dxvk.log').unlink()
            self.assertEqual(review_exit_logs(root,123)['status'],'clean_reported')
