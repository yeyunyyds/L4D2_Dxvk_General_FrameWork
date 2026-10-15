"""Check correspondence, truncation, stale identity and transport UID wrap."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_rt_trace import analyze, PACKET, RECORD


def trace(fault=None):
    records = []
    tick = 0

    def record(kind, uid, data=b'', thread=11):
        nonlocal tick
        tick += 100
        records.append(RECORD.pack(kind, RECORD.size + len(data), uid, thread, 0, tick) + data)

    seq = 0

    def packet(kind, uid, data=b'', frame=0, view=0):
        nonlocal seq
        seq += 1
        version = 2 if fault == 'version' and kind == 1 else 1
        gen = 2 if fault == 'generation' and kind == 5 else 1
        head = PACKET.pack(0x5452444c, version, 104, 104 + len(data), kind, 0,
                           1, 2, 3, gen, 1, frame, view, seq, 7, 0, tick)
        record(1, uid, head + data)

    record(0, 0, struct.pack('<Q', 10000000))
    packet(1, 0xfffffffd)
    packet(2, 0xfffffffe, frame=1)
    camera = struct.pack('<4I34f', 1, 1920, 1200, 0,
                         *([1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0] * 2), 1.0, 1000.0)
    packet(3, 0xffffffff, camera, 1, 1)
    draw = struct.pack('<2Q4I12f', 1, 9, 0, 0, 0, 0, *([1.0] * 12))
    packet(5, 0, draw, 1, 1)
    if fault != 'missing_draw':
        record(2, 1, thread=12 if fault == 'thread' else 11)
        record(5, 1, struct.pack('<I', 0x80004005 if fault == 'failed_draw' else 0))
    packet(6, 2, frame=1, view=1)
    packet(7, 3, frame=1)
    record(3, 4, struct.pack('<QII', 1, 0, 0))
    if fault == 'budget':
        record(9, 0)
    record(8, 0, struct.pack('<4Q2I', 1, 0, 1, 0, 0, int(fault == 'budget')))
    return b''.join(records)


class TraceCases(unittest.TestCase):
    def check_trace(self, data):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'trace.bin'; path.write_bytes(data)
            return analyze(path)

    def test_uid_wrap_is_ordered_by_records_and_frame_identity(self):
        result = self.check_trace(trace())
        self.assertEqual(result['result'], 'pass')
        self.assertEqual(result['labelled_draws'], 1)
        self.assertEqual(result['source_game_queue'], 'unrun')
        self.assertEqual(result['b580_rt'], 'unrun')

    def test_missing_footer_and_partial_record_fail(self):
        data = trace()
        for cut in [1, 8, 31, 72]:
            with self.subTest(cut=cut):
                self.assertEqual(self.check_trace(data[:-cut])['result'], 'fail')

    def test_version_generation_thread_draw_and_budget_fail(self):
        for fault in ['version', 'generation', 'thread', 'missing_draw', 'failed_draw', 'budget']:
            with self.subTest(fault=fault):
                self.assertEqual(self.check_trace(trace(fault))['result'], 'fail')


if __name__ == '__main__':
    unittest.main()
