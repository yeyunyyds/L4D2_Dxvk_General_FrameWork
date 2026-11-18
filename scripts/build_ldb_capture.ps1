# Build the pinned, isolated development trio; do not install into the game.
param(
  [string]$Source = '.research/ldb-capture-source',
  [string]$VcVarsVer = '14.29'
)
$ErrorActionPreference = 'Stop'
if ($VcVarsVer -notmatch '^\d+\.\d+$') { throw 'Invalid MSVC toolset version' }
$repo = Split-Path $PSScriptRoot -Parent
foreach ($scriptName in @('manage_fg_test.ps1','fg_live_hud.ps1')) {
  $scriptTokens = $null
  $scriptErrors = $null
  [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot $scriptName),
    [ref]$scriptTokens, [ref]$scriptErrors) | Out-Null
  if ($scriptErrors.Count) {
    throw ("FG script syntax failed ($scriptName): " + ($scriptErrors -join '; '))
  }
}
& python (Join-Path $PSScriptRoot 'fetch_fsr_assets.py')
if ($LASTEXITCODE -ne 0) { throw 'FSR asset verification failed' }
& python (Join-Path $PSScriptRoot 'prepare_asteria_layer.py')
if ($LASTEXITCODE -ne 0) { throw 'Asteria x64 source preparation failed' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_asteria_layer_x64.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Asteria x64 Vulkan layer build failed' }
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
        $ringExe = Join-Path $sourcePath 'fg_input_ring_cases.exe'
        $ringObj = Join-Path $sourcePath 'fg_input_ring_cases.obj'
        $ringSource = Join-Path $repo 'tests/fg_input_ring_cases.cpp'
        $ringInclude = Join-Path $sourcePath 'bridge/src/server'
        $mathCheck = "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /I`"$mathInclude`" `"$mathSource`" /Fe:`"$mathExe`" /Fo:`"$mathObj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; & `"$mathExe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /I`"$ringInclude`" `"$ringSource`" /Fe:`"$ringExe`" /Fo:`"$ringObj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; & `"$ringExe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; "
      }
      $command = ". .\build_common.ps1; SetupVS -Platform $arch -VcVarsVer $VcVarsVer; $mathCheck & meson setup _captureRelease_$role --buildtype release --backend ninja $extra; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; Copy-Item Directory.Build.Props _captureRelease_$role; & meson compile -C _captureRelease_$role; exit `$LASTEXITCODE"
      & powershell.exe -NoProfile -Command $command
      if ($LASTEXITCODE -ne 0) { throw "Native $role build failed" }
    }
  } finally { Pop-Location }
  & python (Join-Path $PSScriptRoot 'package_ldb_capture.py') --source $sourcePath --build-id $env:L4D2_BRIDGE_BUILD_ID
  if ($LASTEXITCODE -ne 0) { throw 'Development packaging failed' }
  # The user-facing ZIP is deliberately extract-and-run.  Verify that its
  # root contains only bin/ and README.md and that every runtime hash matches.
  $package = Join-Path $repo '.research/dist/ldb-host-capture-dev.zip'
  $fixture = Join-Path $sourcePath '_fgPackageFixture'
  $packageFixture = Join-Path $fixture 'package'
  if (Test-Path $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
  try {
    Expand-Archive -LiteralPath $package -DestinationPath $packageFixture
    $rootNames = @(Get-ChildItem -LiteralPath $packageFixture | ForEach-Object Name | Sort-Object)
    if (($rootNames -join ',') -ne 'bin,README.md') {
      throw "Package root must contain only bin and README.md: $($rootNames -join ', ')"
    }
    $receiptPath = Join-Path $packageFixture 'bin/.l4d2bridge/capture-build.json'
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
      throw 'Package build receipt is missing below bin/.l4d2bridge'
    }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    foreach ($property in $receipt.binaries_sha256.PSObject.Properties) {
      $payload = Join-Path $packageFixture $property.Name
      if (-not (Test-Path -LiteralPath $payload -PathType Leaf) -or
          (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant() -ne
          ([string]$property.Value).ToLowerInvariant()) {
        throw "Package payload checksum mismatch: $($property.Name)"
      }
    }
  } finally {
    if (Test-Path $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
  }
} finally { $env:L4D2_BRIDGE_BUILD_ID = $oldBuildId }
