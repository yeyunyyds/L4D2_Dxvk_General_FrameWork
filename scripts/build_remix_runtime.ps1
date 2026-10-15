param([string]$Source = '.research/rt-remix-runtime')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$sourcePath = (Resolve-Path (Join-Path $repo $Source)).Path
$receipt = Get-Content (Join-Path $sourcePath 'rt-runtime-source.json') -Raw | ConvertFrom-Json
$api = Join-Path $sourcePath 'public/include/remix/remix_c.h'
$smoke = Join-Path $repo 'integration/rt/rt_runtime_smoke.cpp'
if ((Get-FileHash $api -Algorithm SHA256).Hash.ToLower() -ne $receipt.api_sha256 -or
    (Get-FileHash $smoke -Algorithm SHA256).Hash.ToLower() -ne $receipt.smoke_source_sha256) {
  throw 'Runtime API/smoke input changed after preparation'
}
$head = & git -C $sourcePath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head.Trim() -ne $receipt.commit -or $receipt.submodules -ne 'ready') { throw 'Runtime pin/submodules not ready' }
$checks = Join-Path $sourcePath 'checks'
New-Item -ItemType Directory -Force $checks | Out-Null
Push-Location $sourcePath
try {
  . .\build_common.ps1
  SetupBuild -BuildArch x64
  & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT "/external:I$sourcePath/public/include" /external:W0 $smoke "/Fe:$checks/rt-runtime-smoke.exe" "/Fo:$checks/rt-runtime-smoke.obj" /link user32.lib
  if ($LASTEXITCODE -ne 0) { throw 'Static RT diagnostic compilation failed' }
  Write-Host 'RT static-scene diagnostic: COMPILED (GPU execution unrun)'
  $negative = Join-Path $checks ('negative-' + [Guid]::NewGuid().ToString('N'))
  & "$checks/rt-runtime-smoke.exe" "$env:SystemRoot/System32/d3d9.dll" $negative
  $negativeExit = $LASTEXITCODE
  $negativeStatus = Get-Content (Join-Path $negative 'status.json') -Raw | ConvertFrom-Json
  $negativeEvents = Get-Content (Join-Path $negative 'events.jsonl') -Raw
  if ($negativeExit -ne 1 -or $negativeStatus.result -ne 'fail' -or $negativeEvents -notmatch '"stage":"find_api","code":127') {
    throw 'Diagnostic accepted the standard Windows D3D9 DLL or failed at an unexpected stage'
  }
  Write-Host 'RT static-scene diagnostic: STANDARD D3D9 REJECTION PASS (GPU execution unrun)'
  & meson setup _RtRelease --buildtype release --backend ninja -Denable_tracy=false -Ddownload_apics=false
  if ($LASTEXITCODE -ne 0) { throw 'Custom Remix dependency/configuration failed' }
  & meson compile -C _RtRelease -j 3
  if ($LASTEXITCODE -ne 0) { throw 'Custom Remix compilation failed' }
  & meson install -C _RtRelease --no-rebuild --tags output
  if ($LASTEXITCODE -ne 0) { throw 'Custom Remix dependency installation failed' }
  Write-Host 'Pinned custom Remix: RELEASE BUILD PASS (B580 execution unrun)'
} finally { Pop-Location }
