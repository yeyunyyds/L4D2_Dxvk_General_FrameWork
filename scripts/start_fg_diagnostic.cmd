@echo off
setlocal
if not exist "%~dp0left4dead2.exe" (
  echo Extract this diagnostic package beside left4dead2.exe, then run it again.
  pause
  exit /b 1
)
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamExe 2^>nul') do if /i "%%a"=="REG_SZ" set "LDB_FG_STEAM_EXE=%%b"
if not defined LDB_FG_STEAM_EXE set "LDB_FG_STEAM_EXE=%ProgramFiles(x86)%\Steam\steam.exe"
if not exist "%LDB_FG_STEAM_EXE%" (
  echo Steam executable not found.
  pause
  exit /b 1
)
tasklist /fi "imagename eq steam.exe" /nh | find /i "steam.exe" >nul
if not errorlevel 1 (
  echo Exit Steam completely, then run this script again.
  pause
  exit /b 1
)
set "LDB_FG_DIAG_MODE=fsr"
set "LDB_FG_BACKEND=fsr"
set "LDB_FG_ENABLE=1"
if /i "%~1"=="control" (
  set "LDB_FG_DIAG_MODE=control"
  set "LDB_FG_BACKEND=custom"
  set "LDB_FG_ENABLE=0"
)
set "LDB_FG_DIAG_DIR=%~dp0fg-results\startup-%LDB_FG_DIAG_MODE%-%RANDOM%-%RANDOM%"
mkdir "%LDB_FG_DIAG_DIR%"
if not exist "%LDB_FG_DIAG_DIR%" exit /b 1
set "DXVK_LOG_PATH=%LDB_FG_DIAG_DIR%"
set "DXVK_LOG_LEVEL=info"
set "LDB_FG_LOG_DIR=%LDB_FG_DIAG_DIR%"
set "LDB_CAPTURE_DIR="
set "LDB_CAPTURE_TRIGGER="
set "LDB_GPU_REPROJECT=0"
set "LDB_FG_MODE=generated"
set "LDB_FG_CAPTURE_FRAMES=4"
set "LDB_FG_HOTKEYS=1"
set "LDB_FG_AUTO_CAPTURE=1"
set "LDB_FG_PACING_MS=0"
> "%LDB_FG_DIAG_DIR%\startup-info.txt" echo Diagnostic mode: %LDB_FG_DIAG_MODE%
>> "%LDB_FG_DIAG_DIR%\startup-info.txt" echo Bridge, DXVK and FG logs share this directory. No binaries or configuration are changed.
for %%f in ("bin\dxvk_d3d9.dll" "bin\.l4d2bridge\L4D2Bridge64.exe" "bin\.l4d2bridge\L4D2Bridge32.exe" "bin\.l4d2bridge\d3d9vk_x64.dll" "bin\.l4d2bridge\LDBFsrBackend64.dll") do (
  >> "%LDB_FG_DIAG_DIR%\startup-info.txt" echo File: %%~f
  if exist "%~dp0%%~f" (
    certutil -hashfile "%~dp0%%~f" SHA256 >> "%LDB_FG_DIAG_DIR%\startup-info.txt" 2>&1
  ) else (
    >> "%LDB_FG_DIAG_DIR%\startup-info.txt" echo MISSING
  )
)
echo Diagnostic mode: %LDB_FG_DIAG_MODE%
echo Bridge, DXVK and FG logs: %LDB_FG_DIAG_DIR%
echo Try loading the map once. After exiting, upload the complete fg-results directory.
start "" "%LDB_FG_STEAM_EXE%" -applaunch 550
