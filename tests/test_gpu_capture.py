import struct
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_gpu_capture import analyze_gpu
import test_reproject_pipeline as fixtures

class GpuArtifactValidation(unittest.TestCase):
    write=fixtures.ExplicitMatrixPipeline.write
    def setUp(self):
        fixtures.ExplicitMatrixPipeline.setUp(self)
        for e in self.events:
            if e['event']=='draw':e.update(color_id=10,depth_id=20)
            if e['event']=='snapshot':
                e.update(color_id=10,depth_id=20,raw_hdr_color=True,color_preview='reinhard_linear_no_gamma');e['color']['format']=97
                (self.data/(e['prefix']+'-color.rgba16f')).write_bytes(struct.pack('<16e',*([.25,.5,1,1]*4)))
        for frame,seq in [(0,1),(1,4)]:
            d=self.events[seq]
            # Append after completion but resequence, preserving the synthetic observed draws.
            self.events.insert(-1,dict(event='gpu_history' if frame==0 else 'gpu_reprojection',frame=frame,generation=0,device_id=1,prefix=f'{frame}-0-0',matrix_draw_sequence=seq,matrix_register=4,matrix_shader='state-0.bin',matrix_rows=list(struct.unpack_from('<16f',(self.data/d['state']['vs_f32']).read_bytes(),64)),previous_prefix='0-0-0',previous_frame=0,status_counts=[0,4,0,0,0,0],valid_pixels=4,depth_consistent_pixels=4,depth_tolerance=.0001))
        for i,e in enumerate(self.events):e.update(sequence=i,cpu_qpc=i)
        self.write()
        self.raw=self.data/'1-0-0-gpu.f32';self.raw.write_bytes(struct.pack('<32f',*([.25,.5,1,1,1,0,0,1]*4)))
        for suffix in ['gpu-warp.ppm','gpu-valid.ppm']:(self.data/('1-0-0-'+suffix)).write_bytes(b'P6\n2 2\n255\n'+bytes(12))
    def test_matching_gpu_artifact(self):
        r=analyze_gpu(self.data);self.assertEqual(r['gpu_pairs'][0]['pixels_checked'],4);self.assertFalse(r['world_motion_verified'])
    def test_corrupted_gpu_color_is_rejected(self):
        b=bytearray(self.raw.read_bytes());struct.pack_into('<f',b,0,9);self.raw.write_bytes(b)
        with self.assertRaisesRegex(ValueError,'GPU/CPU mismatch'):analyze_gpu(self.data)
    def test_wrong_gpu_frame_pair_is_rejected(self):
        self.events[-2]['previous_frame']=9;self.write()
        with self.assertRaisesRegex(ValueError,'missing frame'):analyze_gpu(self.data)
    def test_truncated_gpu_buffer_is_rejected(self):
        self.raw.write_bytes(self.raw.read_bytes()[:-4])
        with self.assertRaisesRegex(ValueError,'Incomplete GPU'):analyze_gpu(self.data)

if __name__=='__main__':unittest.main()
