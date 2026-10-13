import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_ldb_capture import analyze


class HostObservationEvidence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.events = [
            {'event': 'session', 'start_frame': 0, 'frames_requested': 1},
            {'event': 'snapshot', 'frame': 0, 'prefix': '0-0-0', 'color': {'width': 1, 'height': 1},
             'depth': {'width': 1, 'height': 1}, 'source_layout_restored': True, 'gpu_copy_completed': True},
            {'event': 'present_result', 'frame': 0, 'hresult': 0, 'snapshots': 1},
            {'event': 'completed'},
        ]
        for i, e in enumerate(self.events):
            e.update(device_id=42, sequence=i, generation=0)
        (self.root / '0-0-0-color.ppm').write_bytes(b'P6\n1 1\n255\n\x20\xdc\x40')
        (self.root / '0-0-0-depth.pgm').write_bytes(b'P5\n1 1\n65535\n\x40\x00')
        (self.root / '0-0-0-depth.f32').write_bytes(struct.pack('<f', 0.25))

    def run_analysis(self):
        (self.root / 'events.jsonl').write_text('\n'.join(json.dumps(e) for e in self.events))
        return analyze(self.root)

    def test_complete_files_keep_semantics_unknown(self):
        result = self.run_analysis()
        self.assertEqual(result['semantic_labels'], 'unknown')
        self.assertEqual(result['gpu_fg_execution'], 'unrun')

    def test_present_failure_is_not_success(self):
        self.events[2]['hresult'] = 0x88760868
        with self.assertRaisesRegex(ValueError, 'failed Present'):
            self.run_analysis()

    def test_truncated_raw_depth_is_rejected(self):
        (self.root / '0-0-0-depth.f32').write_bytes(b'\x00')
        with self.assertRaisesRegex(ValueError, 'Incomplete raw depth'):
            self.run_analysis()

    def test_stale_reset_generation_is_rejected(self):
        self.events[1]['generation'] = 1
        with self.assertRaisesRegex(ValueError, 'generation mismatch'):
            self.run_analysis()

    def test_failed_restoration_is_rejected(self):
        self.events[1]['source_layout_restored'] = False
        with self.assertRaisesRegex(ValueError, 'restoration not complete'):
            self.run_analysis()

    def test_early_device_shutdown_is_not_complete(self):
        self.events[-1]['event'] = 'device_destroyed'
        with self.assertRaisesRegex(ValueError, 'did not complete'):
            self.run_analysis()

    def test_metadata_without_snapshots_is_rejected(self):
        self.events.pop(1)
        for i, e in enumerate(self.events): e['sequence'] = i
        with self.assertRaisesRegex(ValueError, 'No usable'):
            self.run_analysis()


if __name__ == '__main__':
    unittest.main()
