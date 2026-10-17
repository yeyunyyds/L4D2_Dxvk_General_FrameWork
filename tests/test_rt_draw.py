import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import analyze_rt_draw as check


class DrawObservation(unittest.TestCase):
    def world_fixture(self):
        identity=[float(i%5==0) for i in range(16)]
        projection=[1.,0,0,0,0,1.,0,0,0,0,-1.0001,-1.,0,0,-7.0007,0]
        declaration=bytes.fromhex('000000000200000000000c00020003000000180001000500000020000100050102000000030000010200100002000301ff00000011000000')
        snapshot=check.SNAPSHOT.pack(0x314f444c,2,42,1,50,11,1,*(identity*2+projection),0x1000,0x2000,0,0,640,360,0,1)
        material=check.MATERIAL.pack(0x480003,3,0,b'LightmappedGeneric_DX9')
        state=check.PIPELINE.pack(0,0,48,1,0,0,1,1,0,0,640,360,0,1,640,360,113,7)+declaration
        events=[(0,0,struct.pack('<IQQQII',2,42,7,10000000,240,30))]
        for i in range(240):
            changed=bytearray(snapshot);struct.pack_into('<Q',changed,16,i+1)
            converted=int(i>=30)
            events.extend([(1,i*2+1,bytes(changed)+material+struct.pack('<8I',2,4,0,0,3,0,1,0)+state),
                           (6,i*2+1,struct.pack('<8I',8 if converted else 1,9 if converted else 0,0,0,0,converted,1,0)),
                           (2,i*2+1,struct.pack('<I',0)),(3,i*2+2,struct.pack('<I',0))])
        events.append((8,0,struct.pack('<6QI',240,240,240,0,0,0,0)))
        return events

    def test_v2_world_control_and_exact_padded_declaration(self):
        r=self.inspect(self.world_fixture())
        self.assertTrue(r['pass'],r['errors'])
        self.assertEqual(r['world_normalization'],'pass')
        self.assertEqual(r['world_conversion']['converted'],210)
        self.assertEqual(r['game_rt'],'unrun')

    def coverage_fixture(self):
        events=self.world_fixture()
        events[0]=(0,0,struct.pack('<IQQQII',3,42,7,10000000,240,30))
        unlit=bytes.fromhex('000000000200000000000c000200030000001800010005000100000004000a01020000000300000102001000020003010300000000000002ff00000011000000')
        post=bytes.fromhex('000000000200000000000c000100050002000000030000010200100002000301ff00000011000000')
        for frame,decl,shader,fmt in ((121,unlit,'UnlitGeneric',0x80503),(122,post,'Neko_Engine_Post',0x80001)):
            index=1+(frame-1)*4;e,u,p=events[index];data=bytearray(p[:check.SNAPSHOT.size])
            if frame==122:
                struct.pack_into('<48f',data,40,*([float(i%5==0) for i in range(16)]*3))
            material=check.MATERIAL.pack(fmt,3,0,shader.encode())
            old_state=list(check.PIPELINE.unpack_from(p,check.SNAPSHOT.size+check.MATERIAL.size+32))
            old_state[2]=32;old_state[-1]=len(decl)//8
            if frame==122:old_state[6]=old_state[7]=0;old_state[-2]=21
            args=p[check.SNAPSHOT.size+check.MATERIAL.size:check.SNAPSHOT.size+check.MATERIAL.size+32]
            events[index]=(e,u,bytes(data)+material+args+check.PIPELINE.pack(*old_state)+decl)
            if frame==122:events[index+1]=(6,u,struct.pack('<8I',9,9,0,0,0,0,0,0))
        return events

    def test_v6_coverage_and_hud_are_bounded_by_phase_and_exact_layout(self):
        events=self.coverage_fixture();report=self.inspect(events)
        self.assertTrue(report['pass'],report['errors'])
        self.assertEqual(report['world_conversion']['hud_replaced'],1)
        old=events.copy();e,u,p=old[0];old[0]=(e,u,struct.pack('<I',2)+p[4:])
        self.assertFalse(self.inspect(old)['pass'])
        for frame in (121,122):
            at=1+(frame-1)*4;e,u,p=events[at];data=bytearray(p)
            data[-len(bytes.fromhex('ff00000011000000'))-3]^=1
            damaged=events.copy();damaged[at]=(e,u,bytes(data))
            self.assertFalse(self.inspect(damaged)['pass'])
        early=events.copy();a,b=1+118*4,1+121*4
        early[a:a+3]=events[b:b+3]
        self.assertFalse(self.inspect(early)['pass'])

    def test_world_report_cannot_hide_bad_uid_control_or_unsafe_material(self):
        original=self.world_fixture()
        wrong=original.copy();wrong[2]=(6,999,wrong[2][2])
        control=original.copy();control[2]=(6,control[2][1],struct.pack('<8I',8,9,0,0,0,1,1,0))
        material=original.copy();at=1+30*4;e,u,p=material[at];data=bytearray(p)
        struct.pack_into('<Q',data,check.SNAPSHOT.size,0x80407);material[at]=(e,u,bytes(data))
        for changed in (wrong,control,material):self.assertFalse(self.inspect(changed)['pass'])

    def fixture(self):
        matrix=[float(i%5==0) for i in range(16)]
        snapshot=check.SNAPSHOT.pack(0x314f444c,1,42,1,50,11,1,*(matrix*3),0x1000,0x2000,0,0,640,360,0,1)
        state=check.PIPELINE.pack(0,0,40,1,0,0,1,1,0,0,640,360,0,1,640,360,21,0)
        events=[(0,0,struct.pack('<IQQQ',1,42,7,10000000))]
        for i in range(120):
            changed=bytearray(snapshot);struct.pack_into('<Q',changed,16,i+1)
            events.extend([(1,i*2+1,bytes(changed)+struct.pack('<8I',2,4,0,0,3,0,1,0)+state),
                           (2,i*2+1,struct.pack('<I',0)),(3,i*2+2,struct.pack('<I',0))])
        events.append((8,0,struct.pack('<6QI',120,120,120,0,0,0,0)))
        return events

    def inspect(self,events):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'trace.bin'
            path.write_bytes(b''.join(check.RECORD.pack(e,32+len(p),u,19,0,i+1)+p for i,(e,u,p) in enumerate(events)))
            return check.analyze(path)

    def test_real_draw_result_correspondence_without_source_queue_claim(self):
        r=self.inspect(self.fixture())
        self.assertTrue(r['pass'],r['errors'])
        self.assertEqual(r['footer']['draws'],120)
        self.assertEqual(r['game_rt'],'unrun')
        self.assertEqual(r['source_queue_execution'],'unrun')
        self.assertEqual(r['source_parallel_preparation'],'unrun')

    def test_result_uid_missing_footer_and_truncation_do_not_pass(self):
        events=self.fixture()
        wrong=events.copy();wrong[2]=(2,1000,wrong[2][2])
        for changed in [wrong,events[:-1],events+[events[1]]]:
            self.assertFalse(self.inspect(changed)['pass'])

    def test_status_cannot_hide_invalid_matrix_or_viewport_mismatch(self):
        events=self.fixture();event,uid,payload=events[1]
        for offset,value in [(40,float('nan')),(40+32*4,0.0)]:
            damaged=bytearray(payload)
            if offset==40+32*4:
                damaged[offset:offset+64]=bytes(64)
            else:
                struct.pack_into('<f',damaged,offset,value)
            changed=events.copy();changed[1]=(event,uid,bytes(damaged))
            self.assertFalse(self.inspect(changed)['pass'])
        damaged=bytearray(payload);struct.pack_into('<I',damaged,check.SNAPSHOT.size+32+10*4,800)
        changed=events.copy();changed[1]=(event,uid,bytes(damaged))
        self.assertFalse(self.inspect(changed)['pass'])
        damaged=bytearray(payload);struct.pack_into('<I',damaged,check.SNAPSHOT.size+32,1)
        changed=events.copy();changed[1]=(event,uid,bytes(damaged))
        self.assertIn('pipeline_query_failure',self.inspect(changed)['errors'])

    def test_profile_failure_duplicate_and_reset_are_explicit(self):
        events=self.fixture();event,uid,payload=events[1]
        damaged=bytearray(payload);struct.pack_into('<I',damaged,36,3)
        changed=events.copy();changed[1]=(event,uid,bytes(damaged))
        self.assertFalse(self.inspect(changed)['pass'])
        changed=events.copy();changed[4]=(1,changed[4][1],payload)
        self.assertFalse(self.inspect(changed)['pass'])
        changed=events.copy();changed[3]=(4,changed[3][1],struct.pack('<I',0))
        self.assertFalse(self.inspect(changed)['pass'])


if __name__=='__main__':unittest.main()
