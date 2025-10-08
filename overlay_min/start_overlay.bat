@echo off
setlocal
cd /d "%~dp0"

rem ---------- Config ----------
set "OVERLAY_WS_PORT=12346"
set "SENDER=%~dp0overlay_sender_auto_single_rank.py"
set "HTML=%~dp0overlay_min\overlay_single_column.html"
set "HOST=%~dp0overlay_host.exe"
rem ----------------------------

echo [BAT] Using port %OVERLAY_WS_PORT%

rem Pick Python launcher
where py >nul 2>&1 && (set "PY=py -3") || (set "PY=python")

rem Kill anything already bound to the port (prevents 10048 bind errors)
for /f "tokens=5" %%P in ('netstat -ano ^| findstr :%OVERLAY_WS_PORT%') do (
  echo [BAT] Killing PID %%P on port %OVERLAY_WS_PORT%
  taskkill /PID %%P /F >nul 2>&1
)

rem Start sender (in its folder) with the port env var
if not exist "%SENDER%" (
  echo [BAT] ERROR: sender not found: %SENDER%
  pause
  exit /b 1
)
pushd "%~dp0"
start "AoE4 Sender" %PY% "%SENDER%"
popd

rem Wait up to ~10s for the port to open
set /a tries=0
:waitloop
set /a tries+=1
timeout /t 1 >nul
netstat -ano | findstr :%OVERLAY_WS_PORT% >nul
if errorlevel 1 (
  if %tries% lss 10 goto waitloop
)
if %tries% geq 10 (
  echo [BAT] WARNING: Sender may not be listening yet; continuing...
) else (
  echo [BAT] Sender is listening on %OVERLAY_WS_PORT%
)

rem Launch overlay host if present, else open HTML directly
if exist "%HOST%" (
  echo [BAT] Starting overlay host...
  start "Overlay Host" "%HOST%"
) else (
  if exist "%HTML%" (
    echo [BAT] Opening HTML in browser...
    start "Overlay HTML" "%HTML%"
  ) else (
    echo [BAT] ERROR: HTML not found: %HTML%
    pause
    exit /b 1
  )
)

echo [BAT] Done.
endlocal
