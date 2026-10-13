#!/usr/bin/env bash
# Workspace-only Windows x64 cross compiler, from authenticated Debian packages.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tool_dir="$repo_dir/.research/tools"
cross_dir="$repo_dir/.research/windows-tools"
bash "$repo_dir/scripts/setup_research_tools.sh" > /dev/null
mkdir -p "$cross_dir/debs"
if [[ -x "$cross_dir/root/usr/bin/x86_64-w64-mingw32-g++-posix" ]]; then
    "$cross_dir/root/usr/bin/x86_64-w64-mingw32-g++-posix" --version
    exit 0
fi
(
    cd -- "$cross_dir/debs"
    apt-get -c "$tool_dir/apt/local.conf" \
        -o Dir::Etc::sourcelist="$tool_dir/apt/sources.list" \
        -o Dir::Etc::sourceparts=- \
        -o Dir::State::Lists="$tool_dir/apt/lists" \
        -o APT::Sandbox::User="$(id -un)" download \
        g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix \
        gcc-mingw-w64-x86-64-posix-runtime gcc-mingw-w64-base \
        mingw-w64-x86-64-dev mingw-w64-common binutils-mingw-w64-x86-64
    sha256sum ./*.deb > SHA256SUMS.local.txt
    for package in ./*.deb; do dpkg-deb -x "$package" "$cross_dir/root"; done
)
"$cross_dir/root/usr/bin/x86_64-w64-mingw32-g++-posix" --version
