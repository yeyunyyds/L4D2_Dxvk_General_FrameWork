#!/usr/bin/env bash
# Install shader tools into the workspace using authenticated Debian repositories.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tool_dir="$repo_dir/.research/tools"
mkdir -p "$tool_dir/apt/lists/partial" "$tool_dir/apt/cache/archives/partial" "$tool_dir/debs"

if [[ -x "$tool_dir/root/usr/bin/glslangValidator" && -x "$tool_dir/root/usr/bin/spirv-val" ]]; then
    "$tool_dir/root/usr/bin/glslangValidator" --version
    "$tool_dir/root/usr/bin/spirv-val" --version
    exit 0
fi

cat > "$tool_dir/apt/sources.list" <<'EOF'
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
EOF
cat > "$tool_dir/apt/local.conf" <<'EOF'
#clear APT::Update::Post-Invoke;
#clear APT::Update::Post-Invoke-Success;
EOF

apt_options=(
    -c "$tool_dir/apt/local.conf"
    -o Dir::Etc::sourcelist="$tool_dir/apt/sources.list"
    -o Dir::Etc::sourceparts=-
    -o Dir::State::Lists="$tool_dir/apt/lists"
    -o Dir::Cache="$tool_dir/apt/cache"
    -o APT::Sandbox::User="$(id -un)"
    -o Debug::NoLocking=true
)
apt-get "${apt_options[@]}" update
(
    cd -- "$tool_dir/debs"
    apt-get "${apt_options[@]}" download glslang-tools spirv-tools
    sha256sum ./*.deb > SHA256SUMS.local.txt
    for package in ./*.deb; do
        dpkg-deb -x "$package" "$tool_dir/root"
    done
)
"$tool_dir/root/usr/bin/glslangValidator" --version
"$tool_dir/root/usr/bin/spirv-val" --version
