"""Create a reviewed, pinned Asteria source tree for the GF x64 Vulkan layer."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

from prepare_research import ROOT, require_source


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checkout(source: Path, repository: str, commit: str) -> None:
    if source.exists():
        require_source(source, commit)
        return
    source.mkdir(parents=True)
    subprocess.run(["git", "init", "-q"], cwd=source, check=True)
    subprocess.run(["git", "config", "core.autocrlf", "false"], cwd=source, check=True)
    subprocess.run(["git", "remote", "add", "origin", repository], cwd=source, check=True)
    subprocess.run(["git", "fetch", "--depth=1", "origin", commit], cwd=source, check=True)
    subprocess.run(["git", "checkout", "--detach", "FETCH_HEAD"], cwd=source, check=True)
    require_source(source, commit)


def prepare(source: Path, output: Path) -> Path:
    spec = json.loads((ROOT / "integration/fg/asteria-source.json").read_text())
    checkout(source, spec["repository"], spec["commit"])
    require_source(source, spec["commit"])
    if output.exists():
        raise RuntimeError(f"Prepared Asteria tree already exists: {output}")
    archive = subprocess.check_output(["git", "archive", "HEAD"], cwd=source)
    output.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as bundle:
        bundle.extractall(output, filter="data")
    patch = ROOT / "integration/fg/asteria-x64.patch"
    subprocess.run(["git", "init", "-q"], cwd=output, check=True)
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=output, check=True)
    subprocess.run(["git", "apply", str(patch)], cwd=output, check=True)
    cmake = output / "Runtime/FrameGeneration/Research/CMakeLists.txt"
    text = cmake.read_text()
    marker = "set(FFX_PLATFORM_NAME x86)"
    if text.count(marker) != 1:
        raise RuntimeError("Unexpected Asteria FidelityFX platform declaration")
    cmake.write_text(text.replace(marker, "set(FFX_PLATFORM_NAME x64)"))
    receipt = {
        "schema": 1,
        "repository": spec["repository"],
        "commit": spec["commit"],
        "patch_sha256": sha256(patch),
        "architecture": "x86_64",
        "route": "L4D2Bridge64 -> Vulkan loader -> GF layer -> DXVK/driver",
    }
    (output / "gf-x64-layer-source.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / ".research/sources/asteria")
    parser.add_argument("--output", type=Path, default=ROOT / ".research/asteria-x64-source")
    args = parser.parse_args()
    try:
        print(prepare(args.source.resolve(), args.output.resolve()))
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
