@echo off
rem Windows entry point for the Lua 5.5 test suite; the work is done by run.ps1.
setlocal
where powershell.exe >nul 2>&1
if errorlevel 1 (
    echo powershell.exe not found; run.ps1 needs Windows PowerShell 5.1 or later.
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
exit /b %errorlevel%
