"""Synthetic-log tests verify failure rejection, not GPU execution."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from analyze_host_capture import analyze


def fixture():
    events = [{"event": "device", "run_id": "current", "frames_requested": 4, "reset_frame": 2,
               "width": 640, "height": 360, "gpu_fg_execution": "unrun"}]
    for frame in range(4):
        generation = int(frame >= 2)
        width, height = (576, 296) if generation else (640, 360)
        if frame == 2:
            events.append({"event": "reset", "run_id": "current", "generation": 1,
                           "before_frame": 2, "width": width, "height": height})
        image = {"width": width, "height": height, "samples": 1, "usage": 1}
        events.append({"event": "frame", "run_id": "current", "render_frame_id": frame,
                       "generation": generation, "color": image.copy(), "depth": image.copy(),
                       "color_copy_verified": True, "depth_copy_verified": True,
                       "source_layout_restored": True, "present_succeeded": True})
    events.append({"event": "completed", "run_id": "current", "frames": 4, "resets": 1, "gpu_fg_execution": "unrun"})
    return events


class CaptureAnalysisTests(unittest.TestCase):
    def test_complete_fixture_has_limited_validation_scope(self):
        result = analyze(fixture(), "current", 0)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["game_capture"], "unrun")
        self.assertEqual(result["gpu_fg_execution"], "unrun")

    def test_failed_process_cannot_pass_with_complete_log(self):
        with self.assertRaisesRegex(ValueError, "exit code"):
            analyze(fixture(), "current", 2)

    def test_stale_or_mixed_runs_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "stale/mixed"):
            analyze(fixture(), "new-run", 0)
        events = fixture()
        events[2]["run_id"] = "previous"
        with self.assertRaisesRegex(ValueError, "stale/mixed"):
            analyze(events, "current", 0)

    def test_missing_frame_or_completion_is_rejected(self):
        events = fixture()
        del events[2]
        with self.assertRaises(ValueError):
            analyze(events, "current", 0)
        with self.assertRaisesRegex(ValueError, "complete"):
            analyze(fixture()[:-1], "current", 0)

    def test_depth_or_layout_failure_is_rejected(self):
        for flag in ("depth_copy_verified", "source_layout_restored", "present_succeeded"):
            events = fixture()
            events[1][flag] = False
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, flag):
                analyze(events, "current", 0)

    def test_wrong_reset_extent_is_rejected(self):
        events = fixture()
        events[3]["width"] = 640
        with self.assertRaisesRegex(ValueError, "extent"):
            analyze(events, "current", 0)

    def test_post_reset_stale_resource_is_rejected(self):
        events = copy.deepcopy(fixture())
        events[4]["depth"]["width"] = 640
        with self.assertRaisesRegex(ValueError, "extent/sample"):
            analyze(events, "current", 0)


if __name__ == "__main__":
    unittest.main()
