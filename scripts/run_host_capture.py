"""Run a bounded Windows diagnostic and preserve new logs plus verified outcomes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

from prepare_research import ROOT
from analyze_host_capture import analyze, read_events


def execute(args):
    if os.name != "nt":
        raise RuntimeError("Run this launcher on Windows; Linux cross compilation is not runtime validation")
    if not args.dxvk.is_file() or not args.exe.is_file():
        raise RuntimeError("DXVK DLL or host executable is missing")
    run_id = uuid.uuid4().hex
    directory = args.output / run_id
    directory.mkdir(parents=True)
    command = [str(args.exe), "--dxvk", str(args.dxvk), "--adapter", args.adapter, "--run-id", run_id,
               "--frames", str(args.frames), "--width", str(args.width), "--height", str(args.height), "--depth-format", args.depth_format]
    manifest = {"run_id": run_id, "command": command,
                "exe_sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                "backend_sha256": hashlib.sha256(args.dxvk.read_bytes()).hexdigest()}
    (directory / "inputs.json").write_text(json.dumps(manifest, indent=2) + "\n")
    environment = os.environ.copy()
    environment["DXVK_LOG_PATH"] = str(directory)
    environment["DXVK_HUD"] = "0"
    with (directory / "capture.jsonl").open("w", encoding="utf-8") as stdout, (directory / "stderr.log").open("w", encoding="utf-8") as stderr:
        try:
            process = subprocess.run(command, cwd=directory, stdout=stdout, stderr=stderr,
                                     env=environment, timeout=args.timeout)
            exit_code = process.returncode
            failure = None
        except subprocess.TimeoutExpired:
            exit_code = None
            failure = "Host timed out; process terminated; capture is incomplete"
    try:
        if failure:
            raise ValueError(failure)
        result = analyze(read_events(directory / "capture.jsonl"), run_id, exit_code)
    except (ValueError, TypeError) as error:
        result = {"status": "failed", "run_id": run_id, "exit_code": exit_code, "error": str(error),
                  "game_capture": "unrun", "gpu_fg_execution": "unrun"}
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({**result, "directory": str(directory)}, indent=2))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk", type=Path, required=True)
    parser.add_argument("--exe", type=Path, default=ROOT / ".research/bin/dxvk_host_capture.exe")
    parser.add_argument("--output", type=Path, default=ROOT / ".research/captures")
    parser.add_argument("--adapter", default="B580")
    parser.add_argument("--depth-format", choices=("auto", "d32", "d24"), default="auto")
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if not 2 <= args.frames <= 1000 or not 128 <= args.width <= 4096 or not 128 <= args.height <= 4096 or not 1 <= args.timeout <= 600:
        parser.error("Frames 2..1000, dimensions 128..4096, timeout 1..600 seconds required")
    for name in ("dxvk", "exe", "output"):
        setattr(args, name, getattr(args, name).resolve())
    try:
        raise SystemExit(execute(args))
    except (RuntimeError, OSError) as error:
        parser.exit(1, f"{error}\n")
