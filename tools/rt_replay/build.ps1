$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force .research/rt-replay/include/remix | Out-Null
Start-Transcript -Path .research/rt-replay/build.log -Force
function Run-ReplayCheck([string]$Command, [string]$Name, [int]$Expected = 0) {
  $log = '.research/rt-replay/' + $Name + '.log'
  & cmd.exe /d /c ($Command + ' > "' + $log + '" 2>&1')
  $code = $LASTEXITCODE
  Get-Content $log
  if ($code -ne $Expected) { throw ($Name + ' failed, exit ' + $code) }
}
try {
  Run-ReplayCheck 'python tools/rt_replay/package.py --prepare-header' 'api-header'
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (!$vs) { throw 'MSVC installation missing' }
  $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
  $prefix = 'call "' + $vcvars + '" x64 -vcvars_ver=14.29 && cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I. /external:I.research/rt-replay/include /external:W0 /FI.research/rt-replay/build-id.h '
  Run-ReplayCheck ($prefix + 'tools/rt_replay/runner.cpp /Fe:.research/rt-replay/rt-replay.exe /Fo:.research/rt-replay/runner.obj /link user32.lib dbghelp.lib') 'compile-runner'
  Run-ReplayCheck ($prefix + 'tools/rt_replay/geometry_cases.cpp /Fe:.research/rt-replay/geometry-cases.exe /Fo:.research/rt-replay/geometry.obj') 'compile-geometry'
  Run-ReplayCheck ($prefix + 'tools/rt_replay/native_cases.cpp /Fe:.research/rt-replay/native-cases.exe /Fo:.research/rt-replay/native.obj /link user32.lib dbghelp.lib') 'compile-native'
  Run-ReplayCheck '.research\rt-replay\geometry-cases.exe' 'geometry-check'
  $base = (Resolve-Path .research/rt-replay).Path
  Run-ReplayCheck ('call "' + $base + '/native-cases.exe" "' + $base + '/native-evidence"') 'native-check'
  Run-ReplayCheck ('call "' + $base + '/rt-replay.exe" --probe "' + $env:SystemRoot + '/System32/d3d9.dll" "' + $base + '/negative"') 'negative-probe' 1
  $negative = Get-Content "$base/negative/status.json" -Raw | ConvertFrom-Json
  $events = Get-Content "$base/negative/events.jsonl" -Raw
  if ($negative.api_probe -ne 'fail' -or $negative.failed_at -ne 'find_api' -or $events -notmatch '"stage":"find_api","code":127' -or $events -notmatch '"stage":"cleanup_complete","code":0') { throw 'System D3D9 negative probe failed' }
  @{ schema=1; commit=$env:GITHUB_SHA; geometry_checks='pass'; api_material_input_checks='pass'; system_nullref_upload='pass'; system_d3d9_negative='pass'; gpu_execution='unrun'; exe_sha256=(Get-FileHash "$base/rt-replay.exe" -Algorithm SHA256).Hash.ToLower(); api_sha256=(Get-FileHash "$base/include/remix/remix_c.h" -Algorithm SHA256).Hash.ToLower(); toolset='14.29'; flags='/O2 /MT /W4 /WX'; architecture='x64' } | ConvertTo-Json | Set-Content -Encoding UTF8 "$base/native-validation.json"
} finally { Stop-Transcript }
