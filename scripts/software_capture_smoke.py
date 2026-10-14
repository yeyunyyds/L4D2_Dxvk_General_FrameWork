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
    environment["LDB_GPU_REPROJECT"] = "1" if getattr(args,"gpu_reproject",False) else "0"
    if getattr(args,"fg_mode",None):
        environment.update(LDB_FG_ENABLE="0" if args.fg_mode=="off" else "1",LDB_FG_MODE="generated" if args.fg_mode=="generated" else "paired",LDB_FG_LOG_DIR="Z:"+str(directory/"fg").replace("/","\\"),LDB_FG_CAPTURE_FRAMES="8",LDB_FG_HOTKEYS="0",LDB_FG_FIXTURE_CASE=getattr(args,"fg_case","normal"))
    if getattr(args,"fg_near_case",None):environment["LDB_FG_FIXTURE_NEAR_CASE"]=args.fg_near_case
    if getattr(args,"fg_auto_capture",False):
        environment.update(LDB_FG_AUTO_CAPTURE="1",LDB_FG_CAPTURE_FRAMES="4",LDB_FG_FIXTURE_CAPTURE_CLOCK="1")
    if getattr(args,"fg_pacing_ms",0):environment["LDB_FG_PACING_MS"]=str(args.fg_pacing_ms)
    if getattr(args, "observer", None) in ("enabled", "waiting", "triggered"):
        environment.update(LDB_CAPTURE_DIR="Z:" + str(directory / "observer").replace("/", "\\"),
                           LDB_CAPTURE_START="0", LDB_CAPTURE_FRAMES=str(args.frames), LDB_CAPTURE_STRIDE=str(getattr(args,"stride",1)), LDB_CAPTURE_SNAPSHOTS="6" if getattr(args, "hdr_scene", False) else "3")
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
    if getattr(args,"fg_odd_extent",False):command += ["--width","641","--height","361"]
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
                if getattr(args,"fg_mode",None):
                    events=read_events(directory/"capture.jsonl")
                    if process.returncode!=0 or not events or events[-1]['event']!='completed':raise ValueError('FG fixture failed; inspect capture.jsonl')
                    selection=[e for e in events if e['event']=='near_fixture_selection']
                    if len(selection)!=1 or selection[0]['mixed']!=(args.fg_near_case!='normal'):raise ValueError('Requested near fixture case not executed')
                    presents=[e for e in events if e['event']=='fixture_present']
                    expected_generated=0 if args.fg_mode=='off' else args.frames-2-(2 if getattr(args,'fg_case','normal') in ('partial','failure','quality','mask_contamination') else 0)
                    expected=args.frames if args.fg_mode in ('off','generated') else args.frames+expected_generated
                    if len(presents)!=expected:raise ValueError('Wrong real Present call count')
                    generated=sum(e['generated_verified'] for e in presents)
                    if generated!=expected_generated:raise ValueError('Generated backbuffer pixels not verified')
                    if args.fg_case!='occlusion' and not all(e.get('dynamic_preserved') for e in presents):raise ValueError('Changing object pixels not verified')
                    result={'dynamic_pixels':('not_exercised_in_parallax_case' if args.fg_case=='occlusion' else 'verified_actual_backbuffer'),'status':'passed','run_id':directory.name,'frames':args.frames,'backend_calls':len(presents),'actual_present_calls':len(presents)-(1 if getattr(args,'fg_case','normal')=='failure' else 0),'generated_backbuffers_verified':generated,'ui_and_near_pixels':'verified_actual_backbuffer','gpu_fg_execution':'executed_fixture' if generated else 'disabled','game_capture':'unrun'}
                    if getattr(args,'fg_case','normal')=='failure' and not any(e['event']=='fixture_failure' and e.get('hresult_preserved') for e in events):raise ValueError('Original backend failure not preserved')
                    if args.fg_mode=='off':
                        if (directory/'fg').exists():raise ValueError('Disabled FG created state/output')
                    else:
                        folders=list((directory/'fg').iterdir())
                        if len(folders)!=1:raise ValueError('FG fixture session count')
                        fgevents=read_events(folders[0]/'fg-events.jsonl')
                        if any(e['event']=='fg_error' for e in fgevents):raise ValueError('FG runtime reported error')
                        if not any(e['event']=='history_invalidated' and e.get('reason')=='reset' for e in fgevents):raise ValueError('FG Reset invalidation unobserved')
                        metrics=[e for e in fgevents if e['event']=='frame_metrics']
                        if len(metrics)!=args.frames:raise ValueError('Missing per-frame telemetry')
                        dispatch=[e['gpu_dispatch_ms'] for e in metrics if e['generated']]
                        if not dispatch or any(x<0 for x in dispatch):raise ValueError('Fixture GPU timestamps unavailable')
                        normal=[e for e in metrics if e['generated'] and not e['captured']]
                        # scene, two near colour copies, depth, UI, final, combined compute/write, restore.
                        expected_submissions=10 if getattr(args,'fg_near_case',None)!='normal' else 8
                        if not normal or any(e['gpu_submissions']!=expected_submissions for e in normal):raise ValueError('Combined dispatch/write submission not verified')
                        result['gpu_timestamps']='verified';result['combined_submission_count']=expected_submissions
                        if any(not e.get('near_colour_mask_valid') for e in metrics if e['generated']):raise ValueError('Near colour mask not validated')
                        result['near_colour_mask']='verified_before_after_HDR'
                        result['near_fixture_case']=getattr(args,'fg_near_case','normal')
                        if getattr(args,'fg_auto_capture',False):
                            captures=[e for e in fgevents if e['event']=='capture']
                            if len(captures)<8 or min(e['frame'] for e in captures)<2:raise ValueError('Delayed periodic capture failed')
                            result['delayed_periodic_captures']=[e['frame'] for e in captures]
                        init=None
                        for event in fgevents:
                            if event['event']=='gpu_initialized':init=event
                            elif event['event']=='frame_metrics' and event.get('quality_total_pixels'):
                                if not init or event['quality_total_pixels']!=init['width']*init['height']:raise ValueError('GPU quality counter missed pixels')
                        rejects=[e for e in fgevents if e['event']=='frame_rejected']
                        if args.fg_case in ('quality','mask_contamination'):
                            if [e['frame'] for e in rejects]!=[2,3] or any(not e.get('original_restored') or e.get('presented') for e in rejects):raise ValueError('Quality-rejected candidate was presented')
                        elif rejects:raise ValueError('Normal fixture was incorrectly rejected')
                        if args.fg_case=='mask_contamination' and any(e['reason']!='excessive_foreground_coverage' for e in rejects):raise ValueError('Contamination rejection reason not validated')
                        if args.fg_near_case=='depth_clear_zero':
                            if any(e['near_depth_pixels']!=e['quality_total_pixels'] or e['foreground_ratio']>.3 or e['near_colour_pixels']==0 for e in metrics if e['generated']):raise ValueError('Cleared depth poisoned foreground mask')
                            result['cleared_depth_diagnostic']='full_depth_coverage_while_foreground_stays_local'
                        if args.fg_case=='motion':
                            if not all(e['motion_interpolated_pixels']>0 for e in metrics if e['generated']):raise ValueError('GPU object motion not exercised')
                            result['textured_object_motion']='verified_midpoint_in_actual_backbuffer'
                        if args.fg_case=='occlusion':result['parallax_occlusion']='verified_revealed_background_and_occluder_width_in_actual_backbuffer'
                        result['whole_frame_quality']='verified';result['rejected_frames']=[e['frame'] for e in rejects]
                        result['fg_events']='verified';result['history_storage']='full_size_gpu'
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
            if getattr(args,"gpu_reproject",False) and args.observer in ("enabled","triggered"):
                from analyze_gpu_capture import analyze_gpu
                folder=next((directory/"observer").iterdir());gpu=analyze_gpu(folder)
                if gpu['status']!='passed_gpu_compute_diagnostic':raise ValueError('GPU compute was not exercised')
                if args.hdr_scene and len(gpu['gpu_pairs'])!=args.frames-2-(1 if args.observer=='triggered' else 0):raise ValueError('Unexpected GPU history count across Reset')
                result['gpu_reprojection_validation']=gpu
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
            # Each run owns a fresh ~770 MB Wine prefix. Retain evidence,
            # but release this reproducible cache after its server exits.
            shutil.rmtree(prefix, ignore_errors=True)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({**result, "directory": str(directory)}, indent=2))
    return 0 if result["status"] in ("passed", "expected_failure") else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--depth-format", choices=("auto", "d32", "d24"), default="auto")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--negative-case", choices=("wrong-arch",))
    parser.add_argument("--stride",type=int,choices=range(1,9),default=1)
    parser.add_argument("--hdr-scene", action="store_true")
    parser.add_argument("--gpu-reproject", action="store_true")
    parser.add_argument("--fg-mode",choices=("paired","generated","off"))
    parser.add_argument("--fg-odd-extent",action="store_true")
    parser.add_argument("--fg-near-case",choices=("normal","no_depth_and_rebind","depth_clear_zero"),default="normal")
    parser.add_argument("--fg-auto-capture",action="store_true")
    parser.add_argument("--fg-pacing-ms",type=int,choices=range(9),default=0)
    parser.add_argument("--fg-case",choices=("normal","partial","failure","quality","mask_contamination","motion","occlusion"),default="normal")
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--observer", choices=("enabled", "disabled", "waiting", "triggered"))
    args = parser.parse_args()
    if not 2 <= args.frames <= 1000:
        parser.error("--frames must be 2..1000")
    if args.hdr_scene and args.observer not in ("enabled","triggered"):
        parser.error("HDR scene validation requires --observer enabled or triggered")
    if args.negative_case and args.validation:
        parser.error("Backend rejection occurs before Vulkan activation; omit --validation")
    try:
        raise SystemExit(smoke(args))
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
