param([string]$Source = '.research/rt-observer-source', [string]$VcVarsVer = '14.29')
$ErrorActionPreference = 'Stop'
if ($VcVarsVer -notmatch '^\d+\.\d+$') { throw 'Invalid toolset selector' }
$repo = Split-Path $PSScriptRoot -Parent
$sourcePath = (Resolve-Path (Join-Path $repo $Source)).Path
$receipt = Get-Content (Join-Path $sourcePath 'rt-source.json') -Raw | ConvertFrom-Json
foreach ($role in @('client', 'server')) {
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
    $include = Join-Path $repo 'integration/rt'
    $command = ". .\build_common.ps1; SetupVS -Platform $arch -VcVarsVer $VcVarsVer; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$protocol`" /Fe:`"$dir/protocol.exe`" /Fo:`"$dir/protocol.obj`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& `"$dir/protocol.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I`"$include`" `"$hostCases`" /Fe:`"$dir/host.exe`" /Fo:`"$dir/host.obj`" /link uuid.lib; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "`$env:LDB_RT_LOG_DIR = '$dir/trace'; & `"$dir/host.exe`"; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "& meson setup _rtRelease_$arch --buildtype release --backend ninja; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; " +
      "Copy-Item Directory.Build.Props _rtRelease_$arch; & meson compile -C _rtRelease_$arch; exit `$LASTEXITCODE"
    Push-Location (Join-Path $sourcePath 'bridge')
    try {
      & powershell.exe -NoProfile -Command $command
      if ($LASTEXITCODE -ne 0) { throw "Native RT foundation $arch failed" }
    } finally { Pop-Location }
  }
} finally { $env:L4D2_BRIDGE_BUILD_ID = $oldId }
