@echo off
setlocal EnableExtensions

rem Fetch the official Lua 5.5 sources for the optional VM bridge.

if not defined LUA_VERSION set "LUA_VERSION=5.5.0"
set "DEPS_DIR=%~dp0.."
set "CANONICAL_DIR=%DEPS_DIR%\lua-5.5"
set "TARGET_DIR=%CANONICAL_DIR%\src"
set "TARBALL=%~1"

if exist "%TARGET_DIR%\lua.h" goto already_present
if defined TARBALL goto check_tarball

set "TARBALL=%TEMP%\lua-%LUA_VERSION%.tar.gz"
set "LUA_URL=https://www.lua.org/ftp/lua-%LUA_VERSION%.tar.gz"
echo Downloading Lua %LUA_VERSION% from %LUA_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; Invoke-WebRequest -Uri $env:LUA_URL -OutFile $env:TARBALL"
if errorlevel 1 goto download_failed

:check_tarball
if not exist "%TARBALL%" goto tarball_missing

if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%"
echo Unpacking "%TARBALL%" into "%DEPS_DIR%" ...
tar -xzf "%TARBALL%" -C "%DEPS_DIR%"
if errorlevel 1 goto unpack_failed

set "EXTRACTED_DIR=%DEPS_DIR%\lua-%LUA_VERSION%"
if not exist "%TARGET_DIR%\lua.h" if not "%EXTRACTED_DIR%"=="%CANONICAL_DIR%" if exist "%EXTRACTED_DIR%" (
    if exist "%CANONICAL_DIR%" goto conflicting_layout
    move "%EXTRACTED_DIR%" "%CANONICAL_DIR%" >nul
    if errorlevel 1 goto layout_failed
)
if not exist "%TARGET_DIR%\lua.h" goto layout_failed

echo.
echo Lua %LUA_VERSION% sources ready at: "%TARGET_DIR%"
echo Configure and build clx with build.bat to use this external Lua source tree.
echo The --dynamic bridge still requires clx_lua.lib built from src\runtime\vm\lua.
exit /b 0

:already_present
echo Lua %LUA_VERSION% already present at "%TARGET_DIR%", nothing to do.
exit /b 0

:download_failed
echo Download failed. Fetch the tarball manually and rerun:
echo   %~nx0 C:\path\to\lua-%LUA_VERSION%.tar.gz
exit /b 1

:conflicting_layout
echo Error: both "%EXTRACTED_DIR%" and "%CANONICAL_DIR%" exist.
exit /b 1

:tarball_missing
echo Error: Lua tarball not found: "%TARBALL%"
exit /b 1

:unpack_failed
echo Error: failed to unpack "%TARBALL%".
exit /b 1

:layout_failed
echo Warning: the archive did not produce the expected layout:
echo   "%TARGET_DIR%\lua.h"
echo Please verify the contents of "%DEPS_DIR%\lua-%LUA_VERSION%".
exit /b 1
