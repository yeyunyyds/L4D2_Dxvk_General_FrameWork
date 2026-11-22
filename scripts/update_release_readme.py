"""Refresh the v1.0.1 release ZIP README without rebuilding any binaries.

Input must be the immutable Windows CI artifact from the successful pinned run.
All non-README ZIP members and the third-party license appendix are preserved.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import zipfile

PINNED_SOURCE = "013b002fa6c0a7e5dfb9e70fac065f0806928c80"
NOTICE_MARKER = b"\n\n---\n\n## Third-party redistributable license notices\n\n"


def replace_readme(source: Path, replacement: Path, target: Path) -> str:
    if source.resolve() == target.resolve():
        raise ValueError("Cannot overwrite the CI artifact")
    expected_prefix = subprocess.check_output(
        ["git", "show", f"{PINNED_SOURCE}:docs/package-readme.md"]
    ).rstrip(b"\r\n")
    new_prefix = replacement.read_bytes().rstrip(b"\r\n")
    if not new_prefix.startswith(b"# L4D2 DXVK General Framework"):
        raise ValueError("Unexpected README header")
    for stale in ("预备版（待验证）", "release candidate — validation pending",
                  "尚未正式发布 v1.0.1", "CI produces a test ZIP for validation"):
        if stale.encode("utf-8") in new_prefix:
            raise ValueError(f"Outdated candidate description: {stale}")
    if b"v1.0.1" not in new_prefix:
        raise ValueError("Version label missing")
    with zipfile.ZipFile(source, "r") as before:
        if before.testzip() is not None:
            raise ValueError("CI ZIP has invalid CRC")
        names = before.namelist()
        if names.count("README.md") != 1 or len(names) != len(set(names)):
            raise ValueError("Missing or duplicate ZIP member")
        original = before.read("README.md")
        if original.count(NOTICE_MARKER) != 1:
            raise ValueError("Third-party license appendix missing or ambiguous")
        prefix, appendix = original.split(NOTICE_MARKER, 1)
        if prefix != expected_prefix:
            raise ValueError("Original README differs from the pinned CI source")
        if not appendix.strip():
            raise ValueError("Third-party license appendix is empty")
        new_readme = new_prefix + NOTICE_MARKER + appendix
        if original == new_readme:
            raise ValueError("No README changes detected")
        with zipfile.ZipFile(target, "w") as after:
            after.comment = before.comment
            for member in before.infolist():
                content = new_readme if member.filename == "README.md" else before.read(member)
                after.writestr(member, content)
    with zipfile.ZipFile(source, "r") as before, zipfile.ZipFile(target, "r") as after:
        if after.testzip() is not None or before.namelist() != after.namelist():
            raise ValueError("Changed ZIP failed validation")
        for name in before.namelist():
            if name == "README.md":
                if after.read(name) != new_readme:
                    raise ValueError("README content was corrupted")
            elif hashlib.sha256(before.read(name)).digest() != hashlib.sha256(after.read(name)).digest():
                raise ValueError(f"Unexpected change in non-README member: {name}")
        if after.read("README.md").split(NOTICE_MARKER, 1)[1] != appendix:
            raise ValueError("License appendix changed")
    return hashlib.sha256(target.read_bytes()).hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--readme", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    sha = replace_readme(args.source, args.readme, args.output)
    Path("SHA256SUMS.txt").write_text(
        f"{sha}  {args.output.name}\n", encoding="utf-8"
    )
    print(f"README-only update verified: {args.output}; SHA256={sha}")
