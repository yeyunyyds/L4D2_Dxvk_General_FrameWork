import copy
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import warnings
import zipfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_replay',ROOT/'tools/rt_replay/replay.py')
replay=importlib.util.module_from_spec(spec);spec.loader.exec_module(replay)


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        identity=[float(i%5==0) for i in range(16)]
        self.vb=struct.pack('<24f',0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,0,0,1,0,0,1,0,0,0)
        self.ib=struct.pack('<3H',0,1,2)
        self.event={'event':'model_rebuild_sample','frame':240,'ordinal':7,'upload_verified':True,'binding_verified':True,
                    'owned_draw':True,'payload_omitted':False,'rebuilt_stride':32,'view':identity,'projection':identity,
                    'applied_world':identity,'kind':2,'topology':4,'draw_args':[0,0,3,0,1,0],'index_bytes':2,
                    'vertices_sha256':replay.sha(self.vb),'indices_sha256':replay.sha(self.ib),'files':[]}
        for suffix,data in [('vertices',self.vb),('indices',self.ib)]:
            name=f'model-7-{suffix}.bin';(self.root/name).write_bytes(data)
            self.event['files'].append({'file':name,'size':len(data),'sha256':replay.sha(data)})
        (self.root/'world-runtime.jsonl').write_text(json.dumps(self.event)+'\n')

    def test_folder_and_nested_zip_same_wire(self):
        capture=replay.Capture(self.root);data,review=replay.scene(capture,[self.event],240);capture.close()
        self.assertEqual(data[:24],b'LDBRPLY1'+struct.pack('<4I',1,640,400,1))
        self.assertEqual(len(data),340);self.assertEqual(review['parts'][0]['triangles'],1)
        archive=self.root/'input.zip'
        with zipfile.ZipFile(archive,'w') as z:
            for path in self.root.glob('*.bin'):z.write(path,'nested/'+path.name)
            z.write(self.root/'world-runtime.jsonl','nested/world-runtime.jsonl')
        capture=replay.Capture(archive);self.addCleanup(capture.close)
        self.assertEqual(replay.scene(capture,[self.event],240)[0],data)

    def test_tampered_payload_rejected(self):
        (self.root/'model-7-vertices.bin').write_bytes(self.vb[:-1]+b'\x01')
        with self.assertRaisesRegex(ValueError,'hash/size'):
            replay.scene(replay.Capture(self.root),[self.event],240)

    def test_partial_cameras_indices_and_states_rejected(self):
        for field,value in [('upload_verified',False),('binding_verified',False),('owned_draw',False),
                            ('payload_omitted',True),('rebuilt_stride',64),('topology',1),('kind',3),
                            ('draw_args',[0,0,3,1,1,0]),('view',[float('nan')]*16)]:
            with self.subTest(field=field):
                event=copy.deepcopy(self.event);event[field]=value
                with self.assertRaises(ValueError):replay.scene(replay.Capture(self.root),[event],240)
        with self.assertRaisesRegex(ValueError,'Duplicate'):replay.scene(replay.Capture(self.root),[self.event,self.event],240)
        event=copy.deepcopy(self.event);event['ordinal']=8;event['view'][12]=1
        with self.assertRaisesRegex(ValueError,'camera'):replay.scene(replay.Capture(self.root),[self.event,event],240)
        bad=struct.pack('<3H',0,1,3);(self.root/'model-7-indices.bin').write_bytes(bad)
        event=copy.deepcopy(self.event);event['indices_sha256']=replay.sha(bad);event['files'][1]['sha256']=replay.sha(bad)
        with self.assertRaisesRegex(ValueError,'Index outside'):replay.scene(replay.Capture(self.root),[event],240)

    def test_strip_winding_and_nonindexed(self):
        event=copy.deepcopy(self.event);event['topology']=5
        data,_=replay.scene(replay.Capture(self.root),[event],240)
        self.assertEqual(struct.unpack('<3I',data[-12:]),(0,1,2))
        event['kind']=1;event['draw_args']=[0,1,0,0,0,0]
        self.assertEqual(replay.scene(replay.Capture(self.root),[event],240)[0],data)

    def test_duplicate_zip_rejected(self):
        archive=self.root/'duplicate.zip'
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            with zipfile.ZipFile(archive,'w') as z:
                z.writestr('world-runtime.jsonl','{}');z.writestr('world-runtime.jsonl','{}')
        with self.assertRaisesRegex(ValueError,'Duplicate'):replay.Capture(archive)

    def test_member_name_no_escape(self):
        capture=replay.Capture(self.root)
        for name in ('../x','a/b','a\\b','C:x','..'):
            with self.assertRaisesRegex(ValueError,'Unsafe'):capture.read(name,10)

    def test_dds_alpha_pattern_and_material_schedule(self):
        data=replay.checker_dds();self.assertEqual(len(data),384);self.assertEqual(data[:4],b'DDS ')
        self.assertEqual(struct.unpack_from('<I',data,4)[0],124)
        self.assertEqual(set(data[131::4]),{0,255})
        self.assertEqual(len(replay.ROUTES)*2+len(replay.MATERIALS),19)

    def test_results_split_and_exclude_model_input(self):
        out=self.root/'result';out.mkdir();(out/'neko.bin').write_bytes(b'private input')
        (out/'review.json').write_text('{}');(out/'a.rgba32f').write_bytes(os.urandom(200000));(out/'b.rgba32f').write_bytes(os.urandom(200000))
        old=replay.MAX_RESULT
        try:
            replay.MAX_RESULT=300000;paths=replay.package_results(out)
        finally:replay.MAX_RESULT=old
        self.assertEqual(len(paths),2)
        for path in paths:
            with zipfile.ZipFile(path) as z:
                self.assertNotIn('neko.bin',z.namelist());self.assertIsNone(z.testzip())
                self.assertEqual(json.loads(z.read('archive-part.json'))['parts'],2)


if __name__=='__main__':unittest.main()
