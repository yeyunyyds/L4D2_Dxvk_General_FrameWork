"""Require current-run pixel verification, contiguous frames and successful Reset."""

import argparse
import json
from pathlib import Path


def analyze(events, run_id, exit_code):
    if exit_code != 0:
        raise ValueError(f"Host process failed with exit code {exit_code}")
    if not events or any(event.get("run_id") != run_id for event in events):
        raise ValueError("Missing events or stale/mixed run IDs")
    if events[0].get("event") != "device" or events[-1].get("event") != "completed":
        raise ValueError("Run did not reach device creation and complete")
    startup, completed = events[0], events[-1]
    expected_frames = startup.get("frames_requested")
    reset_frame = startup.get("reset_frame")
    if type(expected_frames) is not int or not 2 <= expected_frames <= 1000:
        raise ValueError("Invalid requested frame count")
    if type(reset_frame) is not int or reset_frame != -1 and not 1 <= reset_frame < expected_frames:
        raise ValueError("Invalid reset configuration")
    width, height = startup.get("width"), startup.get("height")
    if type(width) is not int or type(height) is not int or min(width, height) < 128:
        raise ValueError("Invalid initial extent")
    generation = 0
    frames = 0
    seen_reset = False
    for event in events[1:-1]:
        kind = event.get("event")
        if kind == "reset":
            if seen_reset or frames != reset_frame or event.get("before_frame") != frames:
                raise ValueError("Reset occurred at the wrong frame")
            generation += 1
            width -= 64
            height -= 64
            if event.get("generation") != generation or event.get("width") != width or event.get("height") != height:
                raise ValueError("Reset generation or extent mismatch")
            seen_reset = True
        elif kind == "frame":
            if event.get("render_frame_id") != frames or event.get("generation") != generation:
                raise ValueError("Missing, duplicated or mis-associated frame")
            if frames == reset_frame and not seen_reset:
                raise ValueError("Required Reset was not observed")
            for flag in ("color_copy_verified", "depth_copy_verified", "source_layout_restored", "present_succeeded"):
                if event.get(flag) is not True:
                    raise ValueError(f"Frame {frames}: {flag} did not pass")
            for name in ("color", "depth"):
                image = event.get(name, {})
                if image.get("width") != width or image.get("height") != height or image.get("samples") != 1:
                    raise ValueError(f"Frame {frames}: {name} resource extent/sample mismatch")
                if type(image.get("usage")) is not int or not image["usage"] & 1:
                    raise ValueError(f"Frame {frames}: {name} lacks Vulkan TRANSFER_SRC usage")
            frames += 1
        else:
            raise ValueError(f"Unexpected event: {kind}")
    if frames != expected_frames or completed.get("frames") != frames or completed.get("resets") != generation:
        raise ValueError("Incomplete frame count or mismatched completion summary")
    if startup.get("gpu_fg_execution") != "unrun" or completed.get("gpu_fg_execution") != "unrun":
        raise ValueError("This capture cannot establish frame-generation execution")
    return {"status": "passed", "run_id": run_id, "frames": frames, "resets": generation,
            "validation_level": "standalone_scene_color_depth_snapshot_readback",
            "game_capture": "unrun", "gpu_fg_execution": "unrun"}


def read_events(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--exit-code", required=True, type=int)
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(read_events(args.log), args.run_id, args.exit_code), indent=2))
    except (ValueError, OSError, TypeError) as error:
        parser.exit(1, f"Capture validation failed: {error}\n")
