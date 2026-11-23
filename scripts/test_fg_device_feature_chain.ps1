# Run the native pNext ABI smoke fixture against the pinned and patched
# Asteria source produced by scripts/prepare_asteria_layer.py.
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Layer = Join-Path $Root '.research/asteria-x64-source/Runtime/FrameGeneration/Layer'
$Vulkan = Join-Path $Layer 'ThirdParty/Vulkan-Headers/include'
$Test = Join-Path $Root 'tests/fg_device_feature_chain_cases.cpp'
$Output = Join-Path $Root '.research/asteria-x64-build/fg_device_feature_chain_cases.exe'
foreach ($File in @($Layer, $Vulkan, $Test)) {
  if (-not (Test-Path -LiteralPath $File)) {
    throw "Native FG chain fixture prerequisite missing: $File"
  }
}
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $VsWhere)) { throw 'vswhere.exe missing' }
$Vs = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $Vs) { throw 'MSVC x64 build tools missing' }
$Aux = Join-Path $Vs 'VC/Auxiliary/Build'
Push-Location $Aux
try {
  & cmd.exe /d /c 'vcvarsall.bat amd64 >nul && set' | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
      Set-Item -Force -Path "Env:$($matches[1])" -Value $matches[2]
    }
  }
} finally { Pop-Location }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC environment missing' }
& cl.exe /nologo /EHsc /std:c++17 /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN "/I$Layer" "/I$Vulkan" $Test "/Fe:$Output"
if ($LASTEXITCODE -ne 0) { throw 'Native Vulkan feature-chain fixture compile failed' }
& $Output
if ($LASTEXITCODE -ne 0) { throw 'Native Vulkan feature-chain fixture failed' }
