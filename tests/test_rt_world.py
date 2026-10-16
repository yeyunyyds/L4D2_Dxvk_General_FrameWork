from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_rt_world import analyze


class ReadbackEvidenceTests(unittest.TestCase):
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
            (root/'world-240-2.rgba32f').write_bytes(struct.pack('<f',1))
            self.assertEqual(analyze(root,previews=False)['runtime_outputs'],'incomplete')

    def test_missing_runtime_or_success_marker_stays_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            self.assertEqual(analyze(root)['runtime_outputs'],'incomplete')
            (root/'world-runtime.jsonl').write_text('{}\n')
            self.assertEqual(analyze(root)['runtime_outputs'],'incomplete')


if __name__=='__main__':unittest.main()
