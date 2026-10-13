"""Build the discovery probe with pinned headers; no SDK import library required."""

import argparse
import json
import os
from pathlib import Path
import subprocess

from prepare_research import ROOT, require_source


def build(args):
    lock = json.loads((ROOT / "research/sources.lock.json").read_text())
    require_source(args.headers, lock["sources"]["vulkan_headers"]["commit"])
    args.output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "tools/vulkan_probe.cpp"
    include = args.headers / "include"
    if os.name == "nt":
        exe = args.output / "vulkan_probe.exe"
        command = [args.compiler or "cl", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                   f"/I{include}", str(source), f"/Fe:{exe}", f"/Fo:{args.output / 'vulkan_probe.obj'}"]
    else:
        exe = args.output / "vulkan_probe"
        command = [args.compiler or "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                   f"-I{include}", str(source), "-ldl", "-o", str(exe)]
    subprocess.run(command, cwd=args.output, check=True)
    print(exe)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, default=ROOT / ".research/sources/vulkan_headers")
    parser.add_argument("--output", type=Path, default=ROOT / ".research/bin")
    parser.add_argument("--compiler")
    args = parser.parse_args()
    args.headers = args.headers.resolve()
    args.output = args.output.resolve()
    try:
        build(args)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
