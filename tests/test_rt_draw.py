import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import analyze_rt_draw as check


class DrawObservation(unittest.TestCase):
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
