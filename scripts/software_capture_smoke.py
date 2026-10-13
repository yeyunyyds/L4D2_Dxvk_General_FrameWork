"""Exercise the Windows EXE through Wine and software Vulkan; never claim B580 support."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import uuid

from analyze_host_capture import analyze, read_events
from prepare_research import ROOT


def smoke(args):
    runtime = ROOT / ".research/software-runtime"
    wine = runtime / "root/usr/lib/wine/wine64"
    server = runtime / "root/usr/lib/wine/wineserver64"
    exe = args.exe.resolve() if getattr(args, "exe", None) else ROOT / ".research/bin/dxvk_host_capture.exe"
    backend = runtime / "d3d9.dll"
    if not all(path.is_file() for path in (wine, exe, backend, runtime / "lvp.json")):
        raise RuntimeError("Run setup_software_runtime.sh and build_host_capture.py first")
    directory = runtime / "captures" / uuid.uuid4().hex
    directory.mkdir(parents=True)
    prefix = directory / "prefix"
    system32 = prefix / "drive_c/windows/system32"
    system32.mkdir(parents=True)
    shutil.copyfile(runtime / "root/usr/x86_64-w64-mingw32/lib/zlib1.dll", system32 / "zlib1.dll")
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith("LDB_CAPTURE_"):
            environment.pop(name)
    environment.update(
        WINEPREFIX=str(prefix), WINEARCH="win64", WINEDEBUG="-all",
        WINEDLLPATH=str(runtime / "root/usr/lib/x86_64-linux-gnu/wine/x86_64-windows"),
        WINEDLLOVERRIDES="d3d9=n;mscoree,mshtml=",
        VK_DRIVER_FILES=str(runtime / "lvp.json"),
        XDG_CACHE_HOME=str(runtime / "cache"),
        DXVK_LOG_PATH="Z:" + str(directory).replace("/", "\\"),
        DXVK_FILTER_DEVICE_NAME="llvmpipe", DXVK_HUD="0",
    )
    environment["LD_LIBRARY_PATH"] = str(runtime / "root/usr/lib/x86_64-linux-gnu/wine/x86_64-unix") + ":" + str(runtime / "root/usr/lib/x86_64-linux-gnu")
    if getattr(args, "observer", None) in ("enabled", "waiting", "triggered"):
        environment.update(LDB_CAPTURE_DIR="Z:" + str(directory / "observer").replace("/", "\\"),
                           LDB_CAPTURE_START="0", LDB_CAPTURE_FRAMES=str(args.frames), LDB_CAPTURE_SNAPSHOTS="6" if getattr(args, "hdr_scene", False) else "3")
        if getattr(args, "hdr_scene", False): environment["LDB_FIXTURE_HDR"] = "1"
        if args.observer == "waiting":
            environment["LDB_CAPTURE_TRIGGER"] = "Z:" + str(directory / "not-created.trigger").replace("/", "\\")
        elif args.observer == "triggered":
            marker = directory / "capture.trigger"
            marker.write_text("")
            environment["LDB_CAPTURE_TRIGGER"] = "Z:" + str(marker).replace("/", "\\")
            environment["LDB_CAPTURE_FRAMES"] = str(args.frames - 1)
    if args.validation:
        layer_dir = directory / "layers"
        layer_dir.mkdir()
        manifest = runtime / "root/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
        if not manifest.is_file():
            raise RuntimeError("Install workspace vulkan-validationlayers before using --validation")
        layer = json.loads(manifest.read_text())
        layer["layer"]["library_path"] = str(runtime / "root/usr/lib/x86_64-linux-gnu/libVkLayer_khronos_validation.so")
        (layer_dir / manifest.name).write_text(json.dumps(layer))
        environment["VK_LAYER_PATH"] = str(layer_dir)
        environment["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
        environment["VK_LOADER_DEBUG"] = "layer"
        environment["DXVK_DEBUG"] = "validation"
    command = [str(wine), str(exe), "--dxvk", str(backend), "--adapter", "llvmpipe",
               "--run-id", directory.name, "--frames", str(args.frames), "--depth-format", args.depth_format]
    if args.negative_case == "wrong-arch":
        import struct
        data = bytearray(160)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3c, 128)
        data[128:132] = b"PE\0\0"
        struct.pack_into("<H", data, 132, 0x14c)
        struct.pack_into("<H", data, 150, 0x2000)
        test_dll = directory / "x86-fixture.dll"
        test_dll.write_bytes(data)
        command[command.index("--dxvk") + 1] = str(test_dll)
    process = None
    with (directory / "xvfb.log").open("w") as xlog:
        display = subprocess.Popen([str(runtime / "root/usr/bin/Xvfb"), "-displayfd", "1", "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
                                   stdout=subprocess.PIPE, stderr=xlog, text=True)
        try:
            readable, _, _ = select.select([display.stdout], [], [], 5)
            number = display.stdout.readline().strip() if readable else ""
            if not number:
                raise RuntimeError(f"Xvfb startup failed; see {directory / 'xvfb.log'}")
            environment["DISPLAY"] = ":" + number
            with (directory / "capture.jsonl").open("w") as stdout, (directory / "stderr.log").open("w") as stderr:
                process = subprocess.run(command, cwd=directory, env=environment, stdout=stdout, stderr=stderr, timeout=90)
            if args.negative_case:
                events = read_events(directory / "capture.jsonl")
                if process.returncode != 2 or len(events) != 1 or events[0].get("event") != "error" or "AMD64 DLL" not in events[0].get("message", ""):
                    raise ValueError("Wrong-architecture backend was not rejected correctly")
                result = {"status": "expected_failure", "run_id": directory.name,
                          "exit_code": process.returncode, "validation_level": "wrong_arch_backend_rejection",
                          "game_capture": "unrun", "gpu_fg_execution": "unrun"}
            else:
                result = analyze(read_events(directory / "capture.jsonl"), directory.name, process.returncode)
            if args.validation:
                messages = (directory / "stderr.log").read_text()
                if "Validation Error" in messages or "VUID-" in messages:
                    raise ValueError("Vulkan validation reported an error; inspect stderr.log")
                if 'Insert instance layer "VK_LAYER_KHRONOS_validation"' not in messages:
                    raise ValueError("Validation layer activation was not observed")
                result["vulkan_validation"] = "passed_no_reported_errors"
            if getattr(args, "observer", None):
                from analyze_ldb_capture import verify_fixture
                result["observer_validation"] = verify_fixture(directory / "observer", args.observer, args.frames, getattr(args, "hdr_scene", False))
            result.update(platform="Wine 10 / Mesa llvmpipe / Xvfb", b580_validation="unrun",
                          exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                          backend_sha256=hashlib.sha256(backend.read_bytes()).hexdigest())
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            result = {"status": "failed", "error": str(error), "run_id": directory.name,
                      "exit_code": process.returncode if process else None,
                      "b580_validation": "unrun", "game_capture": "unrun", "gpu_fg_execution": "unrun"}
        finally:
            # Stop only this freshly created test prefix and this X server.
            subprocess.run([str(server), "-k"], env=environment, timeout=5, check=False, capture_output=True)
            display.terminate()
            display.wait(timeout=5)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({**result, "directory": str(directory)}, indent=2))
    return 0 if result["status"] in ("passed", "expected_failure") else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--depth-format", choices=("auto", "d32", "d24"), default="auto")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--negative-case", choices=("wrong-arch",))
    parser.add_argument("--hdr-scene", action="store_true")
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--observer", choices=("enabled", "disabled", "waiting", "triggered"))
    args = parser.parse_args()
    if not 2 <= args.frames <= 1000:
        parser.error("--frames must be 2..1000")
    if args.hdr_scene and args.observer != "enabled":
        parser.error("HDR scene validation requires --observer enabled")
    if args.negative_case and args.validation:
        parser.error("Backend rejection occurs before Vulkan activation; omit --validation")
    try:
        raise SystemExit(smoke(args))
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
