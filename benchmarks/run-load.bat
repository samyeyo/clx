@echo off
setlocal enabledelayedexpansion

:: ┌─────────────────────────────────────────────┐
:: │  clx — Lua to C++ Native Compiler           │
:: │  benchmarks/run-load.bat · load() benchmarks│
:: └─────────────────────────────────────────────┘
::
:: Benchmarks each script by:
::   1. AOT — native clx compilation (--fast)
::   2. LOAD — compiled with --dynamic, loads the source via load()
::              and runs it through the embedded Lua VM
::
:: Compares against stock Lua 5.5 and LuaJIT.
::
:: Requires: lua, luajit, powershell

:: Navigate to project root (one level up from benchmarks\)
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT_DIR=%%~fI"
cd /d "%ROOT_DIR%"

:: Configuration
set RUNS=10
set WARMUP=1
set TEST_DIR=benchmarks
set TMP_DIR=%TEMP%\clx_bench_load
set LOADER_SRC=%ROOT_DIR%\benchmarks\run_load_shim.lua
set LOADER_EXE=%TMP_DIR%\run_load_shim

set PATH=%PATH%;%ROOT_DIR%\lua
if not exist "%TMP_DIR%" mkdir "%TMP_DIR%"

:: Default to --fast for benchmarking (clx defaults to --size)
if not defined CPPFLAGS set "CPPFLAGS=--fast"

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

:: Check for lua, luajit and powershell
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
where powershell >nul 2>&1
if errorlevel 1 (
    echo Error: 'powershell' is not installed or not in PATH.
    exit /b 1
)

:: Enforce InvariantCulture (dot for decimals) so math doesn't break on French/European locales
set "INV_CULT=[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture;"

::──────────────────────────────────────────────────────────────────────────────
:: Loader shim used for every benchmark (see run_load_shim.lua).
::   Usage: run_load_shim.exe <script.lua>
::──────────────────────────────────────────────────────────────────────────────

echo Building load() shim...
"%CLX_CMD%" "%LOADER_SRC%" --output "%LOADER_EXE%" --dynamic %CPPFLAGS% >nul 2>&1
if not exist "%LOADER_EXE%.exe" (
    echo Error: Failed to compile load shim
    exit /b 1
)

echo.
echo Benchmarking via load() ^(%RUNS% runs, %WARMUP% warmup^)...
echo =============================================================================================================
echo Script                 ^| Lua 5.5           ^| LuaJIT             ^| clx AOT             ^| clx load()             
echo =============================================================================================================

set FOUND_FILES=0

for %%F in (%TEST_DIR%\*.lua) do (
    set "file=%%F"
    set "basename=%%~nF"

    :: Skip *_luajit.lua files, dkjson.lua (dependency) and helper scripts
    if not "!basename:~-7!"=="_luajit" if not "!basename!"=="dkjson" if not "!basename!"=="run-load" if not "!basename!"=="run-hyperfine" if not "!basename!"=="warmup" if not "!basename!"=="run" if not "!basename!"=="run_load_shim" (

    :: Determine luajit file: use *_luajit.lua variant if it exists
    set "luajit_file=%TEST_DIR%\!basename!_luajit.lua"
    if not exist "!luajit_file!" set "luajit_file=!file!"

    :: Multi-module benchmarks
    set "extra="
    if "!basename!"=="canada" set "extra=%TEST_DIR%\dkjson.lua"

    set FOUND_FILES=1
    set "clx_base=%TMP_DIR%\!basename!"
    set "clx_exe=!clx_base!.exe"
    if exist "!clx_exe!" del /f /q "!clx_exe!" >nul 2>&1

    :: AOT compile
    "%CLX_CMD%" --fast "%%F" !extra! --output "!clx_base!" >nul 2>&1

    if not exist "!clx_exe!" (
        powershell -nologo -noprofile -command "%INV_CULT% '{0,-22} | CLX COMPILE FAIL          | -                  | -        | -' -f '!basename!.lua'"
    ) else (
        :: Time lua 5.5, LuaJIT, clx AOT, clx load()
        call :time_engine "lua !file!" avg_lua
        call :time_engine "luajit !luajit_file!" avg_luajit
        call :time_engine "!clx_exe!" avg_clx
        call :time_engine "!LOADER_EXE!.exe !file!" avg_load

        :: Compute speedups via PowerShell
        for /f %%R in ('powershell -nologo -noprofile -command "%INV_CULT% if(!avg_luajit! -gt 0){'{0:F2}x' -f (!avg_lua!/!avg_luajit!)}else{'MAX'}"') do set sp_luajit=%%R
        for /f %%R in ('powershell -nologo -noprofile -command "%INV_CULT% if(!avg_clx! -gt 0){'{0:F2}x' -f (!avg_lua!/!avg_clx!)}else{'MAX'}"') do set sp_clx=%%R
        for /f %%R in ('powershell -nologo -noprofile -command "%INV_CULT% if(!avg_load! -gt 0){'{0:F2}x' -f (!avg_lua!/!avg_load!)}else{'MAX'}"') do set sp_load=%%R

        :: Print row
        powershell -nologo -noprofile -command "%INV_CULT% '{0,-22} | {1,-7}ms (1.00x) | {2,-7}ms ({3,-6}) | {4,-7}ms ({5,-6}) | {6,-7}ms ({7,-6})' -f '!basename!.lua','!avg_lua!','!avg_luajit!','!sp_luajit!','!avg_clx!','!sp_clx!','!avg_load!','!sp_load!'"

        del /f /q "!clx_exe!" >nul 2>&1
    )
    )
)

del /f /q "%LOADER_EXE%.exe" >nul 2>&1

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