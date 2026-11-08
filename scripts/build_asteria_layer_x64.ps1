param(
  [string]$Source = '.research/asteria-x64-source',
  [string]$Output = '.research/asteria-x64-build'
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
$Source = [IO.Path]::GetFullPath((Join-Path $Repo $Source))
$Output = [IO.Path]::GetFullPath((Join-Path $Repo $Output))
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) { throw 'vswhere.exe is missing' }
$Vs = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $Vs) { throw 'MSVC x64 tools are missing' }
$VcAuxiliary = Join-Path $Vs 'VC\Auxiliary\Build'
Push-Location $VcAuxiliary
try {
  & cmd.exe /d /c 'vcvarsall.bat amd64 >nul && set' | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Force -Path "Env:$($matches[1])" -Value $matches[2] }
  }
} finally { Pop-Location }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC environment setup failed' }

$Fg = Join-Path $Source 'Runtime\FrameGeneration'
$Build = Join-Path $Fg 'build\x64'
$Cpu = Join-Path $Fg 'Research\build-cpu-x64'
$Port = Join-Path $Fg 'Port'
$ShaderBuild = Join-Path $Port 'Shaders\build\x64'
New-Item -ItemType Directory -Force $Build,$ShaderBuild,$Output | Out-Null

Push-Location (Join-Path $Fg 'Research')
try {
  & cmake.exe -S . -B $Cpu -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) { throw 'FidelityFX x64 configure failed' }
  & cmake.exe --build $Cpu --target ffx_frameinterpolation_x64 ffx_opticalflow_x64
  if ($LASTEXITCODE -ne 0) { throw 'FidelityFX x64 build failed' }
} finally { Pop-Location }

Push-Location $Port
try {
  $Common = @('/nologo','/c','/EHsc','/std:c++17','/MT','/O2','/DUNICODE','/D_UNICODE','/DNOMINMAX',
    '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR','/Iinclude','/I..\Research\vulkan-headers-v1.3.250\include',
    '/I..\Research\sdk-v1.1.4\sdk\include','/I..\Research\sdk-v1.1.4\sdk\src\shared',
    '/I..\Research\sdk-v1.1.4\sdk\src\backends\shared')
  & cl.exe @Common "/Fo$Build\ffx_vk.obj" 'sources\vk\ffx_vk.cpp'
  if ($LASTEXITCODE -ne 0) { throw 'Asteria Vulkan backend x64 compile failed' }
  & cl.exe @Common "/Fo$Build\AsteriaVkHandles.obj" 'AsteriaVkHandles.cpp'
  if ($LASTEXITCODE -ne 0) { throw 'Asteria handle x64 compile failed' }
  & lib.exe /nologo "/OUT:$Build\AsteriaFGBackend.lib" "$Build\ffx_vk.obj" "$Build\AsteriaVkHandles.obj"
  if ($LASTEXITCODE -ne 0) { throw 'Asteria backend x64 archive failed' }

  $Presenter = @('sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK.cpp',
    'sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_UiComposition.cpp',
    'sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_DebugPacing.cpp',
    'sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_Helpers.cpp')
  foreach ($File in $Presenter) {
    $Name = [IO.Path]::GetFileNameWithoutExtension($File)
    & cl.exe @Common "/Fo$Build\$Name.obj" $File
    if ($LASTEXITCODE -ne 0) { throw "Asteria presenter x64 compile failed: $File" }
  }
  & lib.exe /nologo "/OUT:$Build\AsteriaFGPresenter.lib" ($Presenter | ForEach-Object { "$Build\$([IO.Path]::GetFileNameWithoutExtension($_)).obj" })
  if ($LASTEXITCODE -ne 0) { throw 'Asteria presenter x64 archive failed' }
} finally { Pop-Location }

Push-Location (Join-Path $Port 'Shaders')
try {
  $Sdk = '..\..\Research\sdk-v1.1.4\sdk'
  $ShaderArgs = @('/nologo','/c','/EHsc','/std:c++17','/MT','/O2','/DFFX_FI','/DFFX_OF',
    '/IGenerated',"/I$Sdk\include","/I$Sdk\src\components","/I$Sdk\src\backends\shared")
  $ShaderSources = @(
    @("$Sdk\src\backends\shared\ffx_shader_blobs.cpp",'ffx_shader_blobs.obj'),
    @("$Sdk\src\backends\shared\blob_accessors\ffx_frameinterpolation_shaderblobs.cpp",'ffx_frameinterpolation_shaderblobs.obj'),
    @("$Sdk\src\backends\shared\blob_accessors\ffx_opticalflow_shaderblobs.cpp",'ffx_opticalflow_shaderblobs.obj'))
  foreach ($Pair in $ShaderSources) {
    & cl.exe @ShaderArgs "/Fo$ShaderBuild\$($Pair[1])" $Pair[0]
    if ($LASTEXITCODE -ne 0) { throw "Asteria shader x64 compile failed: $($Pair[0])" }
  }
  & lib.exe /nologo "/OUT:$ShaderBuild\AsteriaFGShaders.lib" ($ShaderSources | ForEach-Object { "$ShaderBuild\$($_[1])" })
  if ($LASTEXITCODE -ne 0) { throw 'Asteria shader x64 archive failed' }
} finally { Pop-Location }

$Def = Join-Path $Output 'AsteriaFrameGenerationLayer64.def'
@'
LIBRARY AsteriaFrameGenerationLayer64
EXPORTS
    vkNegotiateLoaderLayerInterfaceVersion
    vkGetInstanceProcAddr=LayerGetInstanceProcAddr
    vkGetDeviceProcAddr=LayerGetDeviceProcAddr
    vkGetPhysicalDeviceProcAddr=LayerGetPhysicalDeviceProcAddr
    AsteriaValidateNextDispatch
    AsteriaCopyMappedBufferBytes
    AsteriaQueryBoundBufferDescriptor
    AsteriaQueryNativeViewportV1
    AsteriaQueryDrawViewportV1
    AsteriaQueryDrawViewportDiagnosticV1
    AsteriaSubmitFrameGenerationCamera
    AsteriaRegisterFrameInputProviderV1
    AsteriaUnregisterFrameInputProviderV1
    AsteriaQueryFrameInputTicketV1
    AsteriaSubmitFrameInputsV1
'@ | Set-Content -LiteralPath $Def -Encoding ASCII

Push-Location (Join-Path $Fg 'Layer')
try {
  $Dll = Join-Path $Output 'AsteriaFrameGenerationLayer64.dll'
  & cl.exe /nologo /LD /MT /EHs /std:c++17 /O2 /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR `
    '/IThirdParty\Vulkan-Headers\include' '/I..\..\sdk\json' '/I..\Port\include' '/I..\Port\sources' `
    '/I..\Research\sdk-v1.1.4\sdk\include' "/Fe:$Dll" 'AsteriaFrameGenerationLayer.cpp' `
    '..\Port\sources\VulkanShim.cpp' /link "/DEF:$Def" "$Build\AsteriaFGBackend.lib" `
    "$Build\AsteriaFGPresenter.lib" "$ShaderBuild\AsteriaFGShaders.lib" `
    "$Cpu\frameinterpolation\ffx_frameinterpolation_x64.lib" `
    "$Cpu\opticalflow\ffx_opticalflow_x64.lib" user32.lib
  if ($LASTEXITCODE -ne 0) { throw 'GF Asteria Vulkan layer x64 link failed' }
} finally { Pop-Location }

& python (Join-Path $PSScriptRoot 'verify_asteria_layer.py') --dll (Join-Path $Output 'AsteriaFrameGenerationLayer64.dll') --source $Source --output $Output
if ($LASTEXITCODE -ne 0) { throw 'GF Asteria layer verification failed' }
