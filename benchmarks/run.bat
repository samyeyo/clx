@echo off
setlocal enabledelayedexpansion

:: Navigate to project root (one level up from benchmarks\)
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT_DIR=%%~fI"
cd /d "%ROOT_DIR%"

:: Configuration
set RUNS=10
set WARMUP=1
set TEST_DIR=benchmarks

set PATH=%PATH%;%ROOT_DIR%\lua

:: Find clx compiler
if exist "build\bin\Release\clx.exe" (
    set "CLX_CMD=build\bin\Release\clx.exe"
) else if exist "build\bin\clx.exe" (
    set "CLX_CMD=build\bin\clx.exe"
) else if exist "build\clx.exe" (
    set "CLX_CMD=build\clx.exe"
) else if exist "bin\clx.exe" (
    set "CLX_CMD=bin\clx.exe"
) else (
    echo Error: clx.exe not found. Run build.bat first.
    exit /b 1
)

:: Check for lua and luajit
where lua >nul 2>&1
if errorlevel 1 (
    echo Error: 'lua' is not installed or not in PATH.
    exit /b 1
)
where luajit >nul 2>&1
if errorlevel 1 (
    echo Error: 'luajit' is not installed or not in PATH.
    exit /b 1
)

:: Check for PowerShell
where powershell >nul 2>&1
if errorlevel 1 (
    echo Error: 'powershell' is not installed or not in PATH.
    exit /b 1
)

:: Enforce InvariantCulture (dot for decimals) so math doesn't break on French/European locales
set "INV_CULT=[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture;"

echo Starting benchmarks (%RUNS% runs per script^)...
echo ==========================================================================================
echo Script                 ^| Lua 5.5           ^| LuaJIT             ^| clx 
echo ==========================================================================================

set FOUND_FILES=0

for %%F in (%TEST_DIR%\*.lua) do (
    set "file=%%F"
    set "basename=%%~nF"

    :: Skip *_luajit.lua files and dkjson.lua (dependency) — they are not standalone benchmarks
    echo !basename! | findstr /r /i "_luajit$ ^dkjson$" >nul || (

    :: Determine luajit file: use *_luajit.lua variant if it exists
    set "luajit_file=%TEST_DIR%\!basename!_luajit.lua"
    if not exist "!luajit_file!" set "luajit_file=!file!"

    :: Compile with clx
    set FOUND_FILES=1

    :: Multi-module benchmarks
    set "extra="
    if "!basename!"=="canada" set "extra=%TEST_DIR%\dkjson.lua"

    "%CLX_CMD%" "%%F" !extra! --output "!basename!" >nul 2>&1
    
    if not exist "!basename!.exe" (
        powershell -nologo -noprofile -command "%INV_CULT% '{0,-22} | COMPILATION FAILED       | -                  | -' -f '!basename!.lua'"
    ) else (
        :: Time lua 5.5
        call :time_engine "lua !file!" avg_lua

        :: Time LuaJIT
        call :time_engine "luajit !luajit_file!" avg_luajit

        :: Time clx binary
        call :time_engine ".\!basename!.exe" avg_clx

        :: Compute speedups via PowerShell
        for /f %%R in ('powershell -nologo -noprofile -command "%INV_CULT% if(!avg_luajit! -gt 0){'{0:F2}x' -f (!avg_lua!/!avg_luajit!)}else{'MAXx'}"') do set sp_luajit=%%R
        for /f %%R in ('powershell -nologo -noprofile -command "%INV_CULT% if(!avg_clx! -gt 0){'{0:F2}x' -f (!avg_lua!/!avg_clx!)}else{'MAXx'}"') do set sp_clx=%%R

        :: Print row
        powershell -nologo -noprofile -command "%INV_CULT% '{0,-22} | {1,-7}ms (1.00x) | {2,-7}ms ({3,-6}) | {4,-7}ms ({5,-6})' -f '!basename!.lua','!avg_lua!','!avg_luajit!','!sp_luajit!','!avg_clx!','!sp_clx!'"

        del /f /q "!basename!.exe" >nul 2>&1
    )
    )
)

if "%FOUND_FILES%"=="0" (
    echo No .lua scripts found in %TEST_DIR%\.
)

echo ==============================================================================================
echo Benchmarking complete.
exit /b 0

:: -----------------------------------------------------------------------
:: Subroutine: time_engine "command" result_var
:: -----------------------------------------------------------------------
:time_engine
set "_cmd=%~1"
set "_var=%~2"

:: Warmup
if "%WARMUP%"=="1" (
    powershell -nologo -noprofile -command "& {%_cmd%}" >nul 2>&1
)

:: Timed runs
set "_total=0"
for /l %%i in (1,1,%RUNS%) do (
    for /f %%T in ('powershell -nologo -noprofile -command "%INV_CULT% $sw=[System.Diagnostics.Stopwatch]::StartNew(); & {%_cmd%} *>$null 2>$null; $sw.Stop(); '{0:F4}' -f $sw.Elapsed.TotalMilliseconds"') do (
        set "_t=%%T"
        for /f %%A in ('powershell -nologo -noprofile -command "%INV_CULT% '{0:F4}' -f (!_total! + !_t!)"') do set "_total=%%A"
    )
)

for /f %%A in ('powershell -nologo -noprofile -command "%INV_CULT% '{0:F2}' -f (!_total! / %RUNS%)"') do (
    set "%_var%=%%A"
)
exit /b 0