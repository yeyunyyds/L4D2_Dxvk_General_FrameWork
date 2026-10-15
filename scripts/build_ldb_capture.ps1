# Build the pinned, isolated development trio; do not install into the game.
param(
  [string]$Source = '.research/ldb-capture-source',
  [string]$VcVarsVer = '14.29'
)
$ErrorActionPreference = 'Stop'
if ($VcVarsVer -notmatch '^\d+\.\d+$') { throw 'Invalid MSVC toolset version' }
$repo = Split-Path $PSScriptRoot -Parent
& python (Join-Path $PSScriptRoot 'fetch_fsr_assets.py')
if ($LASTEXITCODE -ne 0) { throw 'FSR asset verification failed' }
$sourcePath = (Resolve-Path (Join-Path $repo $Source)).Path
$receipt = Join-Path $sourcePath 'capture-source.json'
if (-not (Test-Path $receipt)) { throw 'Run prepare_ldb_capture.py first' }
$observer = Join-Path $sourcePath 'bridge/src/server/host_capture.h'
$observerHash = (Get-FileHash $observer -Algorithm SHA256).Hash.ToLower()
$expected = (Get-Content $receipt -Raw | ConvertFrom-Json).host_capture_sha256
if ($observerHash -ne $expected) { throw 'Prepared observer differs from receipt' }
$oldBuildId = $env:L4D2_BRIDGE_BUILD_ID
try {
  $env:L4D2_BRIDGE_BUILD_ID = 'ldb-capture-dev+' + (Get-Content $receipt -Raw | ConvertFrom-Json).build_fingerprint.Substring(0, 16)
  Push-Location (Join-Path $sourcePath 'bridge')
  try {
    foreach ($role in @('x64', 'x86', 'x86_server')) {
      $arch = if ($role -eq 'x64') { 'x64' } else { 'x86' }
      $extra = if ($role -eq 'x86_server') { '-Dexperimental_x86_server=true' } else { '' }
      $mathCheck = ''
      if ($role -eq 'x64') {
        $mathExe = Join-Path $sourcePath 'frame_math_cases.exe'
        $mathObj = Join-Path $sourcePath 'frame_math_cases.obj'
        $mathSource = Join-Path $repo 'tests/frame_math_cases.cpp'
        $mathInclude = Join-Path $repo 'integration/ldb'
        $mathCheck = "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /I`"$mathInclude`" `"$mathSource`" /Fe:`"$mathExe`" /Fo:`"$mathObj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; & `"$mathExe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; "
      }
      $command = ". .\build_common.ps1; SetupVS -Platform $arch -VcVarsVer $VcVarsVer; $mathCheck & meson setup _captureRelease_$role --buildtype release --backend ninja $extra; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; Copy-Item Directory.Build.Props _captureRelease_$role; & meson compile -C _captureRelease_$role; exit `$LASTEXITCODE"
      & powershell.exe -NoProfile -Command $command
      if ($LASTEXITCODE -ne 0) { throw "Native $role build failed" }
    }
  } finally { Pop-Location }
  & python (Join-Path $PSScriptRoot 'package_ldb_capture.py') --source $sourcePath --build-id $env:L4D2_BRIDGE_BUILD_ID
  if ($LASTEXITCODE -ne 0) { throw 'Development packaging failed' }
} finally { $env:L4D2_BRIDGE_BUILD_ID = $oldBuildId }
