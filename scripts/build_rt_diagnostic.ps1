$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force .research/preflight-smoke | Out-Null
Start-Transcript -Path .research/preflight-smoke/native.log -Force
try {
  $apiSource = '.research/preflight-smoke/api'
  & git init -q $apiSource
  if ($LASTEXITCODE -ne 0) { throw 'API repository init failed' }
  & git -C $apiSource remote add origin https://github.com/xoxor4d/dxvk-remix.git
  & git -C $apiSource fetch --depth=1 --filter=blob:none origin b73eb23053aa50d3787b0c6c8b4b8ae49962880e
  if ($LASTEXITCODE -ne 0) { throw 'Pinned API fetch failed' }
  & git -C $apiSource checkout FETCH_HEAD -- public/include/remix/remix_c.h
  if ($LASTEXITCODE -ne 0) { throw 'Pinned API header checkout failed' }
  $header = Join-Path $apiSource 'public/include/remix/remix_c.h'
  if ((Get-FileHash $header -Algorithm SHA256).Hash.ToLower() -ne '58867d1dbc7a0980e3a5241750f573c9c8b38cb8f9aee1aba4fcb93bd78ec20e') { throw 'API header hash differs' }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (!$vs) { throw 'MSVC installation missing' }
  $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
  $prefix = 'call "' + $vcvars + '" x64 -vcvars_ver=14.29 && cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /external:I.research/preflight-smoke/api/public/include /external:W0 '
  $command = $prefix + 'integration/rt/rt_runtime_smoke.cpp /Fe:.research/preflight-smoke/rt-runtime-smoke.exe /Fo:.research/preflight-smoke/smoke.obj /link user32.lib dbghelp.lib'
  & cmd.exe /d /c $command
  if ($LASTEXITCODE -ne 0) { throw 'Diagnostic compilation failed' }
  & cmd.exe /d /c ($prefix + '/LD tests/rt_smoke_window_fixture.cpp /Fe:.research/preflight-smoke/window-fixture.dll /Fo:.research/preflight-smoke/window.obj /link user32.lib')
  if ($LASTEXITCODE -ne 0) { throw 'Window hook DLL compilation failed' }
  & cmd.exe /d /c ($prefix + 'tests/rt_smoke_native.cpp /Fe:.research/preflight-smoke/native-tests.exe /Fo:.research/preflight-smoke/native-tests.obj /link user32.lib dbghelp.lib')
  if ($LASTEXITCODE -ne 0) { throw 'Native test compilation failed' }
  $base = (Resolve-Path .research/preflight-smoke).Path
  & "$base/native-tests.exe" "$base/window-fixture.dll" "$base/helper-evidence"
  if ($LASTEXITCODE -ne 0) { throw 'Native diagnostic helper/lifetime tests failed' }
  & "$base/rt-runtime-smoke.exe" "$env:SystemRoot/System32/d3d9.dll" "$base/negative"
  $negativeExit = $LASTEXITCODE
  $status = Get-Content "$base/negative/status.json" -Raw | ConvertFrom-Json
  $events = Get-Content "$base/negative/events.jsonl" -Raw
  if ($negativeExit -ne 1 -or $status.result -ne 'fail' -or $status.cleanup -ne 'complete' -or $status.failed_at -ne 'find_api' -or $events -notmatch '"stage":"find_api","code":127') { throw 'Standard D3D9 rejection or failure cleanup failed' }
  $receipt = @{ schema=1; native_checks='pass'; gpu_execution='unrun'; api_commit='b73eb23053aa50d3787b0c6c8b4b8ae49962880e'; api_sha256=(Get-FileHash $header -Algorithm SHA256).Hash.ToLower(); source_sha256=(Get-FileHash integration/rt/rt_runtime_smoke.cpp -Algorithm SHA256).Hash.ToLower(); exe_sha256=(Get-FileHash "$base/rt-runtime-smoke.exe" -Algorithm SHA256).Hash.ToLower(); compiler_arch='x64'; compiler_toolset='14.29'; optimization='/O2 /MT'; scope='diagnostic_helpers_real_dll_window_lifetime_and_negative_control_not_gpu' }
  $receipt | ConvertTo-Json | Set-Content -Encoding UTF8 "$base/native-validation.json"
} finally { Stop-Transcript }
