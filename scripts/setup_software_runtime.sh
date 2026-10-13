#!/usr/bin/env bash
# Optional Wine + Mesa software Vulkan + Xvfb, confined to the workspace.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tool_dir="$repo_dir/.research/tools"
runtime_dir="$repo_dir/.research/software-runtime"
bash "$repo_dir/scripts/setup_research_tools.sh" > /dev/null
mkdir -p "$runtime_dir/debs"
if [[ ! -x "$runtime_dir/root/usr/lib/wine/wine64" ]]; then
    (
        cd -- "$runtime_dir/debs"
        apt-get -c "$tool_dir/apt/local.conf" \
            -o Dir::Etc::sourcelist="$tool_dir/apt/sources.list" \
            -o Dir::Etc::sourceparts=- \
            -o Dir::State::Lists="$tool_dir/apt/lists" \
            -o APT::Sandbox::User="$(id -un)" download \
            wine64 libwine libz-mingw-w64 mesa-vulkan-drivers xvfb \
            xserver-common=2:21.1.16-1.3+deb13u3 libxfont2
        sha256sum ./*.deb > SHA256SUMS.local.txt
        for package in ./*.deb; do dpkg-deb -x "$package" "$runtime_dir/root"; done
    )
fi
if [[ ! -f "$runtime_dir/root/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" ]]; then
    (
        cd -- "$runtime_dir/debs"
        apt-get -c "$tool_dir/apt/local.conf" \
            -o Dir::Etc::sourcelist="$tool_dir/apt/sources.list" \
            -o Dir::Etc::sourceparts=- \
            -o Dir::State::Lists="$tool_dir/apt/lists" \
            -o APT::Sandbox::User="$(id -un)" download vulkan-validationlayers
        dpkg-deb -x ./vulkan-validationlayers_*.deb "$runtime_dir/root"
    )
fi
python3 - "$runtime_dir" <<'PY'
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile
import urllib.request

root = Path(sys.argv[1])
server = root / 'root/usr/lib/wine/wineserver'
backup = server.with_name('wineserver.debian-original')
if not backup.exists():
    backup.write_bytes(server.read_bytes())
server.write_text('#!/bin/sh\n# Workspace relocation of Debian wineserver launcher.\nexec "$(dirname "$0")/wineserver64" -p0 "$@"\n')
data = root / 'root/usr/share/wine/wine'
data.mkdir(exist_ok=True)
if not (data / 'nls').exists():
    (data / 'nls').symlink_to('../nls')
source = root / 'root/usr/x86_64-w64-mingw32/lib/zlib1.dll'
target = root / 'root/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/zlib1.dll'
if not target.exists():
    shutil.copyfile(source, target)
icd = json.loads((root / 'root/usr/share/vulkan/icd.d/lvp_icd.json').read_text())
icd['ICD']['library_path'] = str(root / 'root/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so')
(root / 'lvp.json').write_text(json.dumps(icd))
archive = root / 'dxvk-2.6.1.tar.gz'
expected = '7ee0bef415910c943d3bda47d9d6821b9c8ca7a74f1e9f6151707d268cf3ce7f'
url = 'https://github.com/doitsujin/dxvk/releases/download/v2.6.1/dxvk-2.6.1.tar.gz'
if not archive.exists():
    archive.write_bytes(urllib.request.urlopen(url, timeout=60).read())
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('DXVK archive digest mismatch; archive preserved, not used')
with tarfile.open(archive) as package:
    dll = package.extractfile('dxvk-2.6.1/x64/d3d9.dll').read()
target = root / 'd3d9.dll'
if target.exists() and target.read_bytes() != dll:
    raise SystemExit('Existing DXVK backend differs; file preserved')
if not target.exists():
    target.write_bytes(dll)
print('Optional software runtime prepared; official DXVK archive hash pinned after initial TLS-authenticated download.')
PY
