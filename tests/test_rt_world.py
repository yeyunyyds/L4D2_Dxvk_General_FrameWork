from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_world import analyze


class ReadbackEvidenceTests(unittest.TestCase):
    def test_alpha_only_normals_are_not_geometry_even_with_nonzero_depth(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            events=[{'event':name,'code':0} for name in ('loaded_pinned_runtime','initialize_api_0_6_4','register_game_device')]
            for frame in (30,120,240):
                for kind in (0,1,2):
                    name=f'world-{frame}-{kind}.rgba32f'
                    pixel=struct.pack('<4f',1.,0.,0.,1.) if kind==1 else struct.pack('<4I',0,0,0,1)
                    (root/name).write_bytes(pixel*(640*400))
                    events.append(dict(event='world_readback',frame=frame,kind=kind,file=name,width=640,height=400,
                                       initialized_to_zero=True,written=True,copy_code=0,hresult=0))
            (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            report=analyze(root,previews=False)
            self.assertEqual(report['runtime_outputs'],'pass')
            self.assertFalse(report['nonzero_world_depth_and_normals'])
            normals=[s for s in report['samples'] if s['kind']==2]
            self.assertTrue(all(s['nonzero_values']>0 and s['nonzero_finite_rgb_values']==0 for s in normals))
            self.assertTrue(all(s['nonzero_rgb_words']==0 and s['first_rgba_u32']==[0,0,0,1] for s in normals))

    def test_api_success_and_unchanged_zero_buffers_do_not_prove_world_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            events=[{'event':name,'code':0} for name in ('loaded_pinned_runtime','initialize_api_0_6_4','register_game_device')]
            for frame in (30,120,240):
                for kind in (0,1,2):
                    name=f'world-{frame}-{kind}.rgba32f'
                    (root/name).write_bytes(bytes(640*400*16))
                    events.append(dict(event='world_readback',frame=frame,kind=kind,file=name,width=640,height=400,
                                       initialized_to_zero=True,written=True,copy_code=0,hresult=0))
            (root/'world-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            report=analyze(root,previews=False)
            self.assertEqual(report['runtime_outputs'],'pass')
            self.assertFalse(report['nonzero_world_depth_and_normals'])
            self.assertEqual(report['geometry_visual_review'],'incomplete')
            self.assertNotEqual(report['game_rt'],'pass')
            gated=[{'event':'startup_raster_policy','code':0},
                   {'event':'rtx.enableRaytracing','code':0,'value':'False'}]+events
            log=root/'world-runtime.jsonl'
            log.write_text('\n'.join(json.dumps(e) for e in gated))
            self.assertIn('missing_gated_rt_on_off_sequence',analyze(root,previews=False)['errors'])
            gated.insert(2,{'event':'raytracing_requested_on','code':0})
            gated.append({'event':'raytracing_requested_off','code':0})
            log.write_text('\n'.join(json.dumps(e) for e in gated))
            self.assertEqual(analyze(root,previews=False)['runtime_outputs'],'pass')
            gated[1]['value']='True';log.write_text('\n'.join(json.dumps(e) for e in gated))
            self.assertIn('startup_rt_not_disabled',analyze(root,previews=False)['errors'])
            (root/'world-240-2.rgba32f').write_bytes(struct.pack('<f',1))
            self.assertEqual(analyze(root,previews=False)['runtime_outputs'],'incomplete')

    def test_missing_runtime_or_success_marker_stays_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            self.assertEqual(analyze(root)['runtime_outputs'],'incomplete')
            (root/'world-runtime.jsonl').write_text('{}\n')
            self.assertEqual(analyze(root)['runtime_outputs'],'incomplete')


if __name__=='__main__':unittest.main()
