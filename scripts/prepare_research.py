"""Fetch immutable research dependencies without modifying existing checkouts."""

import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def require_source(path, commit):
    root = run("git", "rev-parse", "--show-toplevel", cwd=path)
    if Path(root).resolve() != path.resolve():
        raise RuntimeError(f"Not an independent dependency checkout: {path}")
    head = run("git", "rev-parse", "HEAD", cwd=path)
    if head != commit:
        raise RuntimeError(f"Wrong source version at {path}: {head}; expected {commit}")
    if run("git", "status", "--porcelain", "--untracked-files=all", cwd=path):
        raise RuntimeError(f"Dependency has local changes; preserved: {path}")


def prepare(cache):
    lock = json.loads((ROOT / "research/sources.lock.json").read_text())
    cache.mkdir(parents=True, exist_ok=True)
    for name, spec in lock["sources"].items():
        dest = cache / name
        if dest.exists():
            require_source(dest, spec["commit"])
        else:
            run("git", "init", str(dest))
            run("git", "config", "core.autocrlf", "false", cwd=dest)
            run("git", "remote", "add", "origin", spec["repository"], cwd=dest)
            fetch = ["git", "fetch", "--depth=1"]
            if spec.get("sparse_paths"):
                fetch.append("--filter=blob:none")
            run(*fetch, "origin", spec["commit"], cwd=dest)
            if spec.get("sparse_paths"):
                run("git", "sparse-checkout", "init", "--cone", cwd=dest)
                run("git", "sparse-checkout", "set", *spec["sparse_paths"], cwd=dest)
            run("git", "checkout", "--detach", "FETCH_HEAD", cwd=dest)
            require_source(dest, spec["commit"])
        print(f"{name}: {spec['commit']} ({dest})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=ROOT / ".research/sources")
    args = parser.parse_args()
    try:
        prepare(args.cache.resolve())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
