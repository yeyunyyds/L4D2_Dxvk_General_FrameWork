@echo off
setlocal
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamExe 2^>nul') do if /i "%%a"=="REG_SZ" set "LDB_FG_STEAM_EXE=%%b"
if not defined LDB_FG_STEAM_EXE set "LDB_FG_STEAM_EXE=%ProgramFiles(x86)%\Steam\steam.exe"
if not exist "%LDB_FG_STEAM_EXE%" (
  echo Steam executable not found. See README for manual launch.
  pause
  exit /b 1
)
tasklist /fi "imagename eq steam.exe" /nh | find /i "steam.exe" >nul
if not errorlevel 1 (
  echo Exit Steam completely, then run this script again.
  pause
  exit /b 1
)
set "LDB_CAPTURE_DIR="
set "LDB_CAPTURE_TRIGGER="
set "LDB_GPU_REPROJECT=0"
set "LDB_FG_ENABLE=1"
set "LDB_FG_BACKEND=custom"
set "LDB_FG_PRESENTER=1"
set "LDB_FG_MODE=provider"
set "LDB_FG_LOG_DIR=%~dp0fg-results\session-%RANDOM%-%RANDOM%"
set "LDB_FG_CAPTURE_FRAMES=4"
set "LDB_FG_HOTKEYS=1"
set "LDB_FG_AUTO_CAPTURE=1"
set "LDB_FG_PACING_MS=0"
if defined VK_LAYER_PATH (
  set "VK_LAYER_PATH=%~dp0vulkan-layer;%VK_LAYER_PATH%"
) else (
  set "VK_ADD_LAYER_PATH=%~dp0vulkan-layer;%VK_ADD_LAYER_PATH%"
)
set "VK_INSTANCE_LAYERS=VK_LAYER_GF_Asteria_FrameGeneration;%VK_INSTANCE_LAYERS%"
set "ASTERIA_FG_ENABLE=1"
set "ASTERIA_FG_ESTIMATED_INTERNAL=1"
set "ASTERIA_FG_ESTIMATED_DISPATCH=0"
set "ASTERIA_FG_SEPARATE_UI=0"
set "ASTERIA_FG_PROBE_OUTPUT=%LDB_FG_LOG_DIR%\asteria-layer.json"
if not exist "%LDB_FG_LOG_DIR%" mkdir "%LDB_FG_LOG_DIR%"
if not exist "%LDB_FG_LOG_DIR%" exit /b 1
set "DXVK_LOG_PATH=%LDB_FG_LOG_DIR%"
set "DXVK_LOG_LEVEL=info"
echo Starts the x64 Host-controlled Vulkan Layer replacement-swapchain path.
echo Host collects scene/camera timing; the Layer remains the only presenter.
echo Automatic capture: delayed bursts throughout this session. No trigger file.
echo Results: %LDB_FG_LOG_DIR%
start "GF FG Monitor" powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0fg_live_hud.ps1" -LogDirectory "%LDB_FG_LOG_DIR%"
start "" "%LDB_FG_STEAM_EXE%" -applaunch 550 +fps_max 60
