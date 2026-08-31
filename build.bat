@echo off
setlocal enabledelayedexpansion

set BUILD_TYPE=Release
set DO_INSTALL=0

:parse
if "%~1"=="" goto done_parse
if /I "%~1"=="clean" (
    rmdir /s /q build 2>nul
    rmdir /s /q bin 2>nul
    rmdir /s /q lib 2>nul
    exit /b 0
)
if /I "%~1"=="uninstall" goto do_uninstall
if /I "%~1"=="debug"   set BUILD_TYPE=Debug
if /I "%~1"=="install" set DO_INSTALL=1
shift
goto parse
:done_parse

set ARCH_ARGS=
for /f "tokens=1,* delims==" %%a in ('set CLX_ 2^>nul') do (
    set ARCH_ARGS=!ARCH_ARGS! -D%%a=%%b
)

cmake -S . -B build -G "NMake Makefiles" -D CMAKE_BUILD_TYPE=%BUILD_TYPE% !ARCH_ARGS!
if errorlevel 1 exit /b %errorlevel%

:: Build the project
cmake --build build --config %BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

if "%DO_INSTALL%"=="1" (
    cmake --install build --config %BUILD_TYPE%
)
endlocal
exit /b 0

:do_uninstall
if not exist "build\install_manifest.txt" (
    echo Cannot find build\install_manifest.txt. Is the project installed?
    exit /b 1
)
echo Uninstalling...
for /f "usebackq tokens=*" %%f in ("build\install_manifest.txt") do (
    if exist "%%f" del /f /q "%%f"
)
echo Uninstallation complete.
exit /b 0