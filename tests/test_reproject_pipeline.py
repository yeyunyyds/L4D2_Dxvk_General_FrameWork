import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from reproject_capture import reproject
from analyze_capture_series import inventory

class ExplicitMatrixPipeline(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.data=self.root/'data';self.data.mkdir()
        shader=struct.pack('<6I',0xfffe0200,0x03000014,0xc00f0000,0x90e40000,0xa0e40004,0xffff)
        (self.data/'state-0.bin').write_bytes(shader)
        self.mapping={'shader_sha256':hashlib.sha256(shader).hexdigest(),'register_start':4,'storage':'register_rows','evidence':'Synthetic known translation, not a game camera','pixel_center_offset':0.0}
        self.events=[dict(event='session',start_frame=0,frames_requested=2,capture_schema=2,qpc_frequency=1)]
        for frame in range(2):
            m=[1,0,0,.2*frame,0,1,0,0,0,0,1,0,0,0,0,1]
            (self.data/f'state-{frame+1}.bin').write_bytes(struct.pack('<32f',*([0]*16+m)))
            draw_seq=len(self.events)
            self.events.append(dict(event='draw',frame=frame,hresult=0,viewport=dict(x=0,y=0,width=10,height=10,min_z=0,max_z=1),state=dict(vs_shader='state-0.bin',vs_f32=f'state-{frame+1}.bin',vs_registers=8,ps_registers=0)))
            prefix=f'{frame}-0-0'
            self.events.append(dict(event='snapshot',frame=frame,prefix=prefix,last_draw_sequence=draw_seq,color=dict(width=10,height=10,format=44),depth=dict(width=10,height=10),export=dict(width=2,height=2,stride=5,offset=2),source_layout_restored=True,gpu_copy_completed=True))
            self.events.append(dict(event='present_result',frame=frame,hresult=0,snapshots=1))
            (self.data/(prefix+'-color.ppm')).write_bytes(b'P6\n2 2\n255\n'+bytes(12))
            (self.data/(prefix+'-depth.pgm')).write_bytes(b'P5\n2 2\n65535\n'+bytes(8))
            (self.data/(prefix+'-depth.f32')).write_bytes(struct.pack('<4f',.5,.5,.5,.5))
        self.events.append(dict(event='completed'))
        for i,e in enumerate(self.events):e.update(sequence=i,device_id=1,generation=0,cpu_qpc=i)
        self.write()
    def write(self):(self.data/'events.jsonl').write_text('\n'.join(json.dumps(e) for e in self.events))
    def test_inventory_and_sparse_reprojection_keep_hypothesis_status(self):
        info=inventory(self.data);self.assertEqual(info['matrix_candidates'][0]['register_start'],4)
        r=reproject(self.data,'0-0-0','1-0-0',self.mapping,self.root/'output',step=1)
        self.assertEqual(r['valid_points'],4);self.assertEqual(r['depth_consistent_points'],4)
        self.assertFalse(r['world_motion_verified']);self.assertFalse(r['camera_semantics_verified'])
        import csv
        with (self.root/'output/motion-hypothesis.csv').open() as f:
            for row in csv.DictReader(f):self.assertAlmostEqual(float(row['dx']),1.0)
    def test_nonmonotonic_clock_is_rejected(self):
        self.events[-1]['cpu_qpc']=0;self.write()
        with self.assertRaisesRegex(ValueError,'timestamp'):inventory(self.data)
    def test_wrong_shader_mapping_is_rejected(self):
        self.mapping['shader_sha256']='incorrect'
        with self.assertRaisesRegex(ValueError,'shader does not match'):reproject(self.data,'0-0-0','1-0-0',self.mapping,self.root/'output')
    def test_unknown_snapshot_producer_is_rejected(self):
        self.events[2]['last_draw_sequence']=12345;self.write()
        with self.assertRaisesRegex(ValueError,'unknown draw'):inventory(self.data)
    def test_wrong_pair_direction_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'chronological'):reproject(self.data,'1-0-0','0-0-0',self.mapping,self.root/'output')
if __name__=='__main__':unittest.main()
