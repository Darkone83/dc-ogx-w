@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>&1
if %ERRORLEVEL% EQU 0 (
 py -3 ogx_remote.py %*
) else (
 python ogx_remote.py %*
)
if errorlevel 1 pause
endlocal
