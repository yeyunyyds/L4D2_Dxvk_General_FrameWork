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
set "LDB_FG_MODE=paired"
set "LDB_FG_LOG_DIR=%~dp0fg-results"
set "LDB_FG_CAPTURE_FRAMES=4"
set "LDB_FG_HOTKEYS=1"
set "LDB_FG_AUTO_CAPTURE=1"
set "LDB_FG_PACING_MS=2"
if not exist "%LDB_FG_LOG_DIR%" mkdir "%LDB_FG_LOG_DIR%"
if not exist "%LDB_FG_LOG_DIR%" exit /b 1
echo F8: enable/disable. F9: paired/generated preview.
echo Automatic capture: delayed bursts throughout this session. No trigger file.
echo Results: %LDB_FG_LOG_DIR%
start "" "%LDB_FG_STEAM_EXE%" -applaunch 550
