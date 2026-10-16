param([string]$Source = '.research/rt-observer-source', [string]$VcVarsVer = '14.29')
$ErrorActionPreference = 'Stop'
if ($VcVarsVer -notmatch '^\d+\.\d+$') { throw 'Invalid toolset selector' }
$repo = Split-Path $PSScriptRoot -Parent
$sourcePath = (Resolve-Path (Join-Path $repo $Source)).Path
$receipt = Get-Content (Join-Path $sourcePath 'rt-source.json') -Raw | ConvertFrom-Json
foreach ($role in @('client', 'server', 'util')) {
  foreach ($item in $receipt.inputs_sha256.PSObject.Properties) {
    $path = Join-Path $sourcePath "bridge/src/$role/$($item.Name)"
    if (Test-Path $path) {
      if ((Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -ne $item.Value) { throw "Prepared input changed: $path" }
    }
  }
}
$oldId = $env:L4D2_BRIDGE_BUILD_ID
try {
  $env:L4D2_BRIDGE_BUILD_ID = 'ldb-rt-observer+' + $receipt.fingerprint.Substring(0, 16)
  foreach ($arch in @('x64', 'x86')) {
    $dir = Join-Path $sourcePath "checks-$arch"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $protocol = Join-Path $repo 'tests/rt_protocol_cases.cpp'
    $hostCases = Join-Path $repo 'tests/rt_host_cases.cpp'
    $staticCases = Join-Path $repo 'tests/rt_static_draw_cases.cpp'
    $drawPacket = Join-Path $repo 'tests/rt_draw_packet_cases.cpp'
    $drawNative = Join-Path $repo 'tests/rt_draw_native_cases.cpp'
    $transportCases = Join-Path $repo 'tests/rt_transport_cases.cpp'
    $lightingCases = Join-Path $repo 'tests/rt_world_diagnostics_cases.cpp'
    $utilInclude = Join-Path $sourcePath 'bridge/src/util'
    $include = Join-Path $repo 'integration/rt'
    $queueCommand = ''
    $worldLoaderCommand = ''
    if ($arch -eq 'x64') {
      $worldCases = Join-Path $repo 'tests/rt_world_loader_cases.cpp'
      $worldRuntime = Join-Path $repo 'integration/rt/rt_world_runtime.cpp'
      $worldApiDir = Join-Path $sourcePath 'bridge/src/server'
      $worldLoaderCommand = "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" /I`"$worldApiDir`" `"$worldCases`" `"$worldRuntime`" /Fe:`"$dir/world-loader.exe`" /Fo:`"$dir/`" /link bcrypt.lib user32.lib; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; "
    }
    if ($arch -eq 'x86') {
      # The game SDK is x86. Host is x64; do not invent an x64 game ABI.
      $sdk = (Resolve-Path (Join-Path $repo '.research/rt-sdk-source')).Path
      $queueCases = Join-Path $repo 'tests/rt_source_queue_cases.cpp'
      $abiCases = Join-Path $repo 'tests/rt_source_abi.cpp'
      $probeFixture = Join-Path $repo 'tests/rt_game_probe_fixture.cpp'
      $probeCases = Join-Path $repo 'tests/test_rt_game_probe.py'
      $sdkFlags = "/DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /external:I`"$sdk/public`" /external:I`"$sdk/public/tier0`" /external:I`"$sdk/public/tier1`" /external:I`"$sdk/common`" /external:W0"
      $queueCommand = "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT $sdkFlags /I`"$include`" `"$queueCases`" /Fe:`"$dir/source-queue.exe`" /Fo:`"$dir/source-queue.obj`" /link uuid.lib; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
        "`$env:LDB_RT_LOG_DIR = '$dir/source-trace'; & `"$dir/source-queue.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
        "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT $sdkFlags /c /FAs /Fa`"$dir/source-abi.asm`" /Fo:`"$dir/source-abi.obj`" `"$abiCases`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
        "& python `"$repo/scripts/analyze_rt_source_abi.py`" `"$dir/source-abi.asm`" --output `"$dir/source-abi.json`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
        "& cl.exe /nologo /std:c++17 /EHsc /GR /W4 /WX /O2 /MT `"$probeFixture`" /Fe:`"$dir/game-probe-fixture.exe`" /Fo:`"$dir/game-probe-fixture.obj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
        "`$env:RT_GAME_PROBE_FIXTURE = '$dir/game-probe-fixture.exe'; `$env:RT_GAME_PROBE_EVIDENCE = '$dir/game-probe.json'; " +
        "& python `"$probeCases`" -v; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; "
    }
    $command = ". .\build_common.ps1; SetupVS -Platform $arch -VcVarsVer $VcVarsVer; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$protocol`" /Fe:`"$dir/protocol.exe`" /Fo:`"$dir/protocol.obj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/protocol.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$drawPacket`" /Fe:`"$dir/draw-packet.exe`" /Fo:`"$dir/draw-packet.obj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/draw-packet.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$utilInclude`" `"$transportCases`" /Fe:`"$dir/transport.exe`" /Fo:`"$dir/transport.obj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/transport.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$lightingCases`" /Fe:`"$dir/world-lighting.exe`" /Fo:`"$dir/world-lighting.obj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/world-lighting.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" /I`"$repo/tests`" `"$drawNative`" /Fe:`"$dir/draw-native.exe`" /Fo:`"$dir/draw-native.obj`" /link bcrypt.lib user32.lib; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/draw-native.exe`" `"$dir/draw-observation`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& python `"$repo/scripts/analyze_rt_draw.py`" `"$dir/draw-observation`" --output `"$dir/draw-observation-review.json`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$staticCases`" /Fe:`"$dir/static-draw.exe`" /Fo:`"$dir/static-draw.obj`" /link user32.lib; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/static-draw.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$hostCases`" /Fe:`"$dir/host.exe`" /Fo:`"$dir/host.obj`" /link uuid.lib; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "`$env:LDB_RT_LOG_DIR = '$dir/trace'; & `"$dir/host.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      $queueCommand + $worldLoaderCommand +
      "& meson setup _rtRelease_$arch --buildtype release --backend ninja; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "Copy-Item Directory.Build.Props _rtRelease_$arch; & meson compile -C _rtRelease_$arch; exit `$LASTEXITCODE"
    Push-Location (Join-Path $sourcePath 'bridge')
    try {
      & powershell.exe -NoProfile -Command $command
      if ($LASTEXITCODE -ne 0) { throw "Native RT foundation $arch failed" }
    } finally { Pop-Location }
  }
} finally { $env:L4D2_BRIDGE_BUILD_ID = $oldId }
