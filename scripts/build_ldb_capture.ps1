# Build the pinned, isolated development trio; do not install into the game.
param(
  [string]$Source = '.research/ldb-capture-source',
  [string]$VcVarsVer = '14.29'
)
$ErrorActionPreference = 'Stop'
if ($VcVarsVer -notmatch '^\d+\.\d+$') { throw 'Invalid MSVC toolset version' }
$repo = Split-Path $PSScriptRoot -Parent
$sourcePath = (Resolve-Path (Join-Path $repo $Source)).Path
$receipt = Join-Path $sourcePath 'capture-source.json'
if (-not (Test-Path $receipt)) { throw 'Run prepare_ldb_capture.py first' }
$observer = Join-Path $sourcePath 'bridge/src/server/host_capture.h'
$observerHash = (Get-FileHash $observer -Algorithm SHA256).Hash.ToLower()
$expected = (Get-Content $receipt -Raw | ConvertFrom-Json).host_capture_sha256
if ($observerHash -ne $expected) { throw 'Prepared observer differs from receipt' }
$oldBuildId = $env:L4D2_BRIDGE_BUILD_ID
try {
  $env:L4D2_BRIDGE_BUILD_ID = 'ldb-capture-dev+' + $observerHash.Substring(0, 16)
  Push-Location (Join-Path $sourcePath 'bridge')
  try {
    foreach ($role in @('x64', 'x86', 'x86_server')) {
      $arch = if ($role -eq 'x64') { 'x64' } else { 'x86' }
      $extra = if ($role -eq 'x86_server') { '-Dexperimental_x86_server=true' } else { '' }
      $command = ". .\build_common.ps1; SetupVS -Platform $arch -VcVarsVer $VcVarsVer; & meson setup _captureRelease_$role --buildtype release --backend ninja $extra; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; Copy-Item Directory.Build.Props _captureRelease_$role; & meson compile -C _captureRelease_$role; exit `$LASTEXITCODE"
      & powershell.exe -NoProfile -Command $command
      if ($LASTEXITCODE -ne 0) { throw "Native $role build failed" }
    }
  } finally { Pop-Location }
  & python (Join-Path $PSScriptRoot 'package_ldb_capture.py') --source $sourcePath --build-id $env:L4D2_BRIDGE_BUILD_ID
  if ($LASTEXITCODE -ne 0) { throw 'Development packaging failed' }
} finally { $env:L4D2_BRIDGE_BUILD_ID = $oldBuildId }
