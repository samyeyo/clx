@echo off

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
if /I "%~1"=="debug"   set BUILD_TYPE=Debug
if /I "%~1"=="install" set DO_INSTALL=1
shift
goto parse
:done_parse

cmake -S . -B build -G "NMake Makefiles" -D CMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

:: Build the project
cmake --build build --config %BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

if "%DO_INSTALL%"=="1" (
    cmake --install build --config %BUILD_TYPE%
)