import math
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from capture_math import inverse,mul,shader_usage,project_previous

class ProjectionDiagnostics(unittest.TestCase):
    def test_static_camera_has_zero_motion(self):
        m=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
        v=dict(x=0,y=0,width=100,height=80,min_z=0,max_z=1)
        self.assertEqual(project_previous(25,60,.4,inverse(m),m,v,v),(25,60,.4))
    def test_known_translation_and_viewport_mapping(self):
        m=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];p=m.copy();p[3]=.2
        v=dict(x=0,y=0,width=100,height=80,min_z=0,max_z=.1)
        x,y,z=project_previous(25,60,.04,inverse(m),p,v,v)
        self.assertAlmostEqual(x,35);self.assertAlmostEqual(y,60);self.assertAlmostEqual(z,.04)
    def test_singular_matrix_is_rejected(self):
        with self.assertRaises(ValueError):inverse([0]*16)
    def test_matrix_opcode_expands_register_block(self):
        words=[0xfffe0200,0x03000014,0xc00f0000,0x90e40000,0xa0e40004,0xffff]
        info=shader_usage(struct.pack('<6I',*words))
        self.assertEqual(info['matrix_starts'],[4]);self.assertEqual(info['read_constants'],[4,5,6,7])
    def test_shader_def_constants_override_device_state(self):
        words=[0xfffe0200,0x05000051,0xa00f0004,0,0,0,0,0x03000014,0xc00f0000,0x90e40000,0xa0e40004,0xffff]
        info=shader_usage(struct.pack('<12I',*words));self.assertEqual(info['matrix_starts'],[])
    def test_truncated_bytecode_cannot_be_used(self):
        with self.assertRaises(ValueError):shader_usage(struct.pack('<3I',0xfffe0200,0x03000014,0xffff))
if __name__=='__main__':unittest.main()
