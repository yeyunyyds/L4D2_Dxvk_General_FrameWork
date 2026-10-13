"""Compile and validate the pinned FSR Vulkan FG/optical-flow shader permutations.

This does not build the SDK host library or execute frame generation on a GPU.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import uuid

from prepare_research import ROOT, require_source


def checked_tool(command):
    return subprocess.check_output([command, "--version"], text=True).strip()


def shader_jobs(source):
    base = source / "sdk/src/backends/vk/shaders"
    fg = sorted((base / "frameinterpolation").glob("*.glsl"))
    optical = sorted((base / "opticalflow").glob("*.glsl"))
    if len(fg) != 11 or len(optical) != 7:
        raise RuntimeError(f"Expected 11 FG + 7 optical-flow passes, got {len(fg)} + {len(optical)}")
    jobs = []
    for shader in fg:
        for low_res, jitter, inverted in itertools.product((0, 1), repeat=3):
            options = {
                "FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS": low_res,
                "FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS": jitter,
                "FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH": inverted,
            }
            jobs.append((shader, options, f"low{low_res}-jitter{jitter}-inv{inverted}"))
    for shader in optical:
        for hdr in (0, 1):
            jobs.append((shader, {"FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT": hdr}, f"hdr{hdr}"))
    return jobs


def verify(args):
    lock = json.loads((ROOT / "research/sources.lock.json").read_text())
    require_source(args.source, lock["sources"]["fidelityfx"]["commit"])
    tools = {"glslang": checked_tool(args.glslang), "spirv_val": checked_tool(args.spirv_val)}
    jobs = shader_jobs(args.source)
    run_dir = args.output / uuid.uuid4().hex
    run_dir.mkdir(parents=True)
    gpu = args.source / "sdk/include/FidelityFX/gpu"

    def compile_job(job):
        shader, options, variant = job
        output = run_dir / f"{shader.stem}-{variant}.spv"
        command = [args.glslang, "-V", "--target-env", "vulkan1.2", "-S", "comp", "-Os",
                   "-DFFX_GLSL=1", "-DFFX_GPU=1", "-DFFX_HALF=0", f"-I{gpu}", f"-I{gpu / shader.parent.name}"]
        if shader.parent.name == "frameinterpolation":
            command += [
                "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
                "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
                "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
                "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
                "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
            ]
        command += [f"-D{name}={value}" for name, value in options.items()]
        command += [str(shader), "-o", str(output)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        record = {"shader": str(shader.relative_to(args.source)), "variant": options,
                  "compile_exit": compiled.returncode, "validation_exit": None,
                  "status": "failed", "command": command}
        log = compiled.stdout + compiled.stderr
        if compiled.returncode == 0 and output.is_file():
            validated = subprocess.run([args.spirv_val, "--target-env", "vulkan1.2", str(output)],
                                       capture_output=True, text=True)
            record["validation_exit"] = validated.returncode
            log += validated.stdout + validated.stderr
            if validated.returncode == 0:
                record.update(status="passed", spirv_sha256=hashlib.sha256(output.read_bytes()).hexdigest())
        (run_dir / f"{output.stem}.log").write_text(log, encoding="utf-8")
        return record

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        records = list(executor.map(compile_job, jobs))
    passed = sum(record["status"] == "passed" for record in records)
    report = {"source_commit": lock["sources"]["fidelityfx"]["commit"],
              "scope": "FP32 GLSL compile and SPIR-V validation; no GPU execution or host build",
              "tools": tools, "passed": passed, "failed": len(records) - passed,
              "total": len(records), "results": records}
    (run_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{passed}/{len(records)} compiled and validated; report: {run_dir / 'report.json'}")
    for record in records:
        if record["status"] != "passed":
            print(f"FAILED: {record['shader']} {record['variant']}")
    return 0 if passed == len(records) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / ".research/sources/fidelityfx")
    parser.add_argument("--output", type=Path, default=ROOT / ".research/shaders")
    parser.add_argument("--glslang", default="glslangValidator")
    parser.add_argument("--spirv-val", default="spirv-val")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error("--jobs must be between 1 and 16")
    try:
        raise SystemExit(verify(args))
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
