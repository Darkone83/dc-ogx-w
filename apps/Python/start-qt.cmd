@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv-qt\Scripts\python.exe" (
  py -3 -m venv .venv-qt
  if errorlevel 1 goto failed
)
.venv-qt\Scripts\python.exe -m pip install --disable-pip-version-check -r requirements.txt
if errorlevel 1 goto failed
.venv-qt\Scripts\python.exe ogx_remote.py %*
if errorlevel 1 goto failed
exit /b 0
:failed
echo.
echo Could not start DC-OGX-W. Python 3.10-3.13 and an initial internet connection are required.
pause
exit /b 1
