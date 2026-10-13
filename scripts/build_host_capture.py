"""Build the standalone Windows x64 DXVK snapshot diagnostic."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from prepare_research import ROOT, require_source


def generate_interop(dxvk, output):
    """Keep upstream ABI declarations intact; replace only internal includes."""
    source = dxvk / "src/d3d9/d3d9_interfaces.h"
    data = source.read_text(encoding="utf-8")
    includes = ['#include "d3d9_include.h"', '#include "../vulkan/vulkan_loader.h"']
    for line in includes:
        if data.count(line) != 1:
            raise RuntimeError("Unexpected DXVK interop includes; header not generated")
    data = data.replace(includes[0], "#include <windows.h>\n#include <d3d9.h>")
    data = data.replace(includes[1], "#include <vulkan/vulkan.h>")
    license_text = (dxvk / "LICENSE").read_text(encoding="utf-8")
    notice = "// Adapted DXVK header: original ABI retained; internal includes replaced.\n"
    notice += "// Upstream: doitsujin/dxvk v2.6.1. Original license follows.\n"
    notice += "\n".join("// " + line for line in license_text.splitlines()) + "\n"
    output.write_text(notice + data, encoding="utf-8")
    return hashlib.sha256(source.read_bytes()).hexdigest()


def build(args):
    lock = json.loads((ROOT / "research/sources.lock.json").read_text())
    require_source(args.dxvk, lock["sources"]["dxvk"]["commit"])
    require_source(args.headers, lock["sources"]["vulkan_headers"]["commit"])
    args.output.mkdir(parents=True, exist_ok=True)
    header_hash = generate_interop(args.dxvk, args.output / "dxvk_interop.h")
    exe = args.output / "dxvk_host_capture.exe"
    includes = [args.output, args.headers / "include"]
    source = ROOT / "tools/dxvk_host_capture.cpp"
    environment = os.environ.copy()
    if os.name == "nt":
        command = [args.compiler or "cl", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                   *[f"/I{path}" for path in includes], str(source), f"/Fe:{exe}",
                   f"/Fo:{args.output / 'dxvk_host_capture.obj'}", "/link", "user32.lib", "uuid.lib"]
    else:
        compiler = args.compiler or shutil.which("x86_64-w64-mingw32-g++")
        if not compiler:
            compiler = str(ROOT / ".research/windows-tools/root/usr/bin/x86_64-w64-mingw32-g++-posix")
        command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-cast-function-type", "-static", "-static-libgcc", "-static-libstdc++", "-municode",
                   *[f"-I{path}" for path in includes], str(source), "-luser32", "-luuid", "-o", str(exe)]
        environment["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
    subprocess.run(command, cwd=args.output, env=environment, check=True)
    record = {"target": "Windows AMD64", "dxvk_commit": lock["sources"]["dxvk"]["commit"],
              "vulkan_headers_commit": lock["sources"]["vulkan_headers"]["commit"],
              "upstream_interop_sha256": header_hash, "command": command,
              "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "runtime_validation": "unrun"}
    (args.output / "host-capture-build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(exe)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk", type=Path, default=ROOT / ".research/sources/dxvk")
    parser.add_argument("--headers", type=Path, default=ROOT / ".research/sources/vulkan_headers")
    parser.add_argument("--output", type=Path, default=ROOT / ".research/bin")
    parser.add_argument("--compiler")
    args = parser.parse_args()
    for name in ("dxvk", "headers", "output"):
        setattr(args, name, getattr(args, name).resolve())
    try:
        build(args)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
