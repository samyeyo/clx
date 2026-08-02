@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: Locate project root and compiler.
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT_DIR=%%~fI"

if exist "%ROOT_DIR%\build\bin\Release\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\bin\Release\clx.exe"
) else if exist "%ROOT_DIR%\build\Release\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\Release\clx.exe"
) else if exist "%ROOT_DIR%\build\bin\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\bin\clx.exe"
) else if exist "%ROOT_DIR%\build\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\clx.exe"
) else if exist "%ROOT_DIR%\bin\clx.exe" (
    set "COMPILER=%ROOT_DIR%\bin\clx.exe"
) else (
    echo Error: clx.exe not found. Run build.bat first.
    exit /b 1
)

set "TMP_DIR=%TEMP%\clx-load-test-%RANDOM%"
set "BIN=%TMP_DIR%\run_via_load.exe"
set "LOG=%TMP_DIR%\run_via_load.log"

mkdir "%TMP_DIR%" >nul 2>&1
if not exist "%TMP_DIR%" (
    echo [FAIL] Could not create temporary directory: %TMP_DIR%
    exit /b 1
)

cd /d "%ROOT_DIR%"
if errorlevel 1 (
    echo [FAIL] Could not change to project root: %ROOT_DIR%
    goto :fail
)

echo Using compiler: %COMPILER%
echo Compiling tests\run_via_load.lua with --dynamic...
"%COMPILER%" --dynamic tests\run_via_load.lua --output "%BIN%"
if errorlevel 1 goto :compile_fail
if not exist "%BIN%" goto :compile_fail

echo Running load-mode test suite...
"%BIN%" >"%LOG%" 2>&1
set "RUN_EXIT=!errorlevel!"
type "%LOG%"

:: run_via_load.lua reports aggregate failures in its summary. Check the
:: summary as well as the process status because older harness versions did
:: not propagate fail_count through their exit status.
findstr /c:"fail  : 0" "%LOG%" >nul 2>&1
if !RUN_EXIT! equ 0 if !errorlevel! equ 0 (
    echo [PASS] load-mode test suite
    goto :success
)

echo [FAIL] load-mode test suite
if !RUN_EXIT! neq 0 echo Harness exit code: !RUN_EXIT!
goto :fail

:compile_fail
echo [FAIL] Could not compile the load-mode test harness.
goto :fail

:success
rmdir /s /q "%TMP_DIR%" >nul 2>&1
exit /b 0

:fail
rmdir /s /q "%TMP_DIR%" >nul 2>&1
exit /b 1
