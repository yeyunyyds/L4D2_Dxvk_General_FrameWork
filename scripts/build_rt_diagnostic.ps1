$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force .research/preflight-smoke | Out-Null
Start-Transcript -Path .research/preflight-smoke/native.log -Force
function Invoke-LoggedNative([string]$Command, [string]$Name, [int]$Expected = 0) {
  $log = '.research/preflight-smoke/' + $Name + '.log'
  & cmd.exe /d /c ($Command + ' > "' + $log + '" 2>&1')
  $result = $LASTEXITCODE
  Get-Content $log
  if ($result -ne $Expected) { throw ($Name + ' failed with exit ' + $result) }
}
try {
  $apiSource = '.research/preflight-smoke/api'
  Invoke-LoggedNative ('git init -q ' + $apiSource) 'api-init'
  Invoke-LoggedNative ('git -C ' + $apiSource + ' remote add origin https://github.com/xoxor4d/dxvk-remix.git') 'api-origin'
  Invoke-LoggedNative ('git -C ' + $apiSource + ' fetch --depth=1 origin b73eb23053aa50d3787b0c6c8b4b8ae49962880e') 'api-fetch'
  Invoke-LoggedNative ('git -C ' + $apiSource + ' checkout FETCH_HEAD -- public/include/remix/remix_c.h') 'api-header'
  $header = Join-Path $apiSource 'public/include/remix/remix_c.h'
  if ((Get-FileHash $header -Algorithm SHA256).Hash.ToLower() -ne '58867d1dbc7a0980e3a5241750f573c9c8b38cb8f9aee1aba4fcb93bd78ec20e') { throw 'API header hash differs' }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (!$vs) { throw 'MSVC installation missing' }
  $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
  $prefix = 'call "' + $vcvars + '" x64 -vcvars_ver=14.29 && cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /external:I.research/preflight-smoke/api/public/include /external:W0 '
  $command = $prefix + 'integration/rt/rt_runtime_smoke.cpp /Fe:.research/preflight-smoke/rt-runtime-smoke.exe /Fo:.research/preflight-smoke/smoke.obj /link user32.lib dbghelp.lib'
  Invoke-LoggedNative $command 'compile-smoke'
  Invoke-LoggedNative ($prefix + '/LD tests/rt_smoke_window_fixture.cpp /Fe:.research/preflight-smoke/window-fixture.dll /Fo:.research/preflight-smoke/window.obj /link user32.lib') 'compile-window'
  Invoke-LoggedNative ($prefix + 'tests/rt_smoke_native.cpp /Fe:.research/preflight-smoke/native-tests.exe /Fo:.research/preflight-smoke/native-tests.obj /link user32.lib dbghelp.lib') 'compile-tests'
  $base = (Resolve-Path .research/preflight-smoke).Path
  Invoke-LoggedNative ('call "' + $base + '/native-tests.exe" "' + $base + '/window-fixture.dll" "' + $base + '/helper-evidence"') 'helper-checks'
  Invoke-LoggedNative ('call "' + $base + '/rt-runtime-smoke.exe" "' + $env:SystemRoot + '/System32/d3d9.dll" "' + $base + '/negative"') 'negative-control' 1
  $status = Get-Content "$base/negative/status.json" -Raw | ConvertFrom-Json
  $events = Get-Content "$base/negative/events.jsonl" -Raw
  if ($status.result -ne 'fail' -or $status.cleanup -ne 'complete' -or $status.failed_at -ne 'find_api' -or $events -notmatch '"stage":"find_api","code":127') { throw 'Standard D3D9 rejection or failure cleanup failed' }
  $receipt = @{ schema=1; native_checks='pass'; gpu_execution='unrun'; api_commit='b73eb23053aa50d3787b0c6c8b4b8ae49962880e'; api_sha256=(Get-FileHash $header -Algorithm SHA256).Hash.ToLower(); source_sha256=(Get-FileHash integration/rt/rt_runtime_smoke.cpp -Algorithm SHA256).Hash.ToLower(); exe_sha256=(Get-FileHash "$base/rt-runtime-smoke.exe" -Algorithm SHA256).Hash.ToLower(); compiler_arch='x64'; compiler_toolset='14.29'; optimization='/O2 /MT'; scope='diagnostic_helpers_real_dll_window_lifetime_and_negative_control_not_gpu' }
  $receipt | ConvertTo-Json | Set-Content -Encoding UTF8 "$base/native-validation.json"
} finally { Stop-Transcript }
exit 0
