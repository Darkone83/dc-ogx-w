@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  echo Python 3 was not found. Install Python 3 with the Windows py launcher, then run this file again.
  pause
  exit /b 1
)
if not exist ".venv-web\Scripts\python.exe" (
  py -3 -m venv .venv-web
  if errorlevel 1 goto failed
)
".venv-web\Scripts\python.exe" -c "import aiohttp" >nul 2>nul
if errorlevel 1 (
  ".venv-web\Scripts\python.exe" -m pip install -r requirements-web.txt
  if errorlevel 1 goto failed
)
".venv-web\Scripts\python.exe" ogx_remote.py --prompt-host web
if errorlevel 1 goto failed
exit /b 0
:failed
echo.
echo The browser companion could not start. Read the error above.
pause
exit /b 1
