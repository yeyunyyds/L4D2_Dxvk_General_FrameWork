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
  # Exercise the scripts from the finished ZIP against an isolated fake game.
  # This verifies install, tamper refusal and byte-exact restore on Windows.
  $package = Join-Path $repo '.research/dist/ldb-host-capture-dev.zip'
  $fixture = Join-Path $sourcePath '_fgInstallerFixture'
  $packageFixture = Join-Path $fixture 'package'
  $gameFixture = Join-Path $fixture 'game'
  if (Test-Path $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
  try {
    New-Item -ItemType Directory -Path (Join-Path $gameFixture 'bin/.l4d2bridge') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $gameFixture 'left4dead2.exe') -Value 'fixture' -Encoding ASCII
    $installerFiles = @(
      'bin/dxvk_d3d9.dll',
      'bin/.l4d2bridge/L4D2Bridge64.exe',
      'bin/.l4d2bridge/L4D2Bridge32.exe',
      'bin/.l4d2bridge/LDBFsrBackend64.dll',
      'bin/.l4d2bridge/d3d9vk_x64.dll',
      'vulkan-layer/AsteriaFrameGenerationLayer64.dll',
      'vulkan-layer/VK_LAYER_GF_Asteria_FrameGeneration.json',
      'vulkan-layer/gf-asteria-layer-build.json'
    )
    $originalHashes = @{}
    foreach ($relative in $installerFiles) {
      $path = Join-Path $gameFixture $relative
      New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
      Set-Content -LiteralPath $path -Value ('original:' + $relative) -Encoding ASCII
      $originalHashes[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    Expand-Archive -LiteralPath $package -DestinationPath $packageFixture
    $manager = Join-Path $packageFixture 'manage_fg_test.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $manager -Action Install -Game $gameFixture
    if ($LASTEXITCODE -ne 0) { throw 'FG installer fixture install failed' }
    foreach ($relative in $installerFiles) {
      $installed = Join-Path $gameFixture $relative
      $packaged = Join-Path $packageFixture $relative
      if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
          (Get-FileHash -LiteralPath $packaged -Algorithm SHA256).Hash) {
        throw "FG installer fixture mismatch: $relative"
      }
    }
    $tampered = Join-Path $gameFixture $installerFiles[0]
    Set-Content -LiteralPath $tampered -Value 'later-user-change' -Encoding ASCII
    # This invocation must fail. PowerShell 7 may promote a non-zero native
    # process exit to a terminating error when ErrorActionPreference is Stop,
    # so temporarily allow it and inspect the exit code ourselves.
    $savedErrorActionPreference = $ErrorActionPreference
    try {
      $ErrorActionPreference = 'Continue'
      & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $manager -Action Restore -Game $gameFixture
      $tamperRestoreExit = $LASTEXITCODE
    } finally {
      $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($tamperRestoreExit -eq 0 -or -not (Test-Path (Join-Path $gameFixture '.ldb-fg-test-backup'))) {
      throw 'FG restore did not preserve a tampered installation and its backup'
    }
    Copy-Item -LiteralPath (Join-Path $packageFixture $installerFiles[0]) -Destination $tampered -Force
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $manager -Action Restore -Game $gameFixture
    if ($LASTEXITCODE -ne 0) { throw 'FG installer fixture restore failed' }
    foreach ($relative in $installerFiles) {
      if ((Get-FileHash -LiteralPath (Join-Path $gameFixture $relative) -Algorithm SHA256).Hash -ne
          $originalHashes[$relative]) { throw "FG restore differs: $relative" }
    }
    if (Test-Path (Join-Path $gameFixture '.ldb-fg-test-backup')) {
      throw 'FG restore left its backup directory behind'
    }
  } finally {
    if (Test-Path $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
  }
} finally { $env:L4D2_BRIDGE_BUILD_ID = $oldBuildId }
