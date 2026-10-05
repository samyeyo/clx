@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

:: Configuration - same names and defaults
if "%LUASOCKET_VERSION%"=="" set LUASOCKET_VERSION=3.1.0
if "%CLX_INCLUDE%"=="" set CLX_INCLUDE=..\..\include
set BUILD=build
set SRC=%BUILD%\luasocket-%LUASOCKET_VERSION%\src
set LUASOCKET_URL=https://github.com/lunarmodules/luasocket/archive/refs/tags/v%LUASOCKET_VERSION%.tar.gz

:: Detect clx (installed layout first, then in-tree build)
set CLX=..\..\bin\clx.exe
if exist "%CLX%" goto clx_found
set CLX=..\..\build\clx.exe
if exist "%CLX%" goto clx_found
echo clx.exe not found.
echo Please build clx first by running "build.bat" in the repository root.
exit /b 1
:clx_found
for %%I in ("%CLX%") do set "CLX=%%~fI"

if exist "%CLX_INCLUDE%\lua.h" goto headers_ok
echo Error: %CLX_INCLUDE%\lua.h not found (set CLX_INCLUDE to clx's include directory.)
exit /b 1
:headers_ok

:: Detect C compiler: clx on Windows links with cl, so prefer it
set CSTYLE=msvc
if not "%CC%"=="" goto cc_custom
where cl.exe >nul 2>&1
if not errorlevel 1 (set "CC=cl.exe" & goto cc_ready)
set CSTYLE=gnu
where gcc >nul 2>&1
if not errorlevel 1 (set "CC=gcc" & goto cc_ready)
where clang >nul 2>&1
if not errorlevel 1 (set "CC=clang" & goto cc_ready)
echo Error: no C compiler found.
echo Run this from an "x64 Native Tools Command Prompt" ^(cl.exe^), or install gcc or clang.
exit /b 1

:cc_custom
set CSTYLE=gnu
for %%I in ("%CC%") do set CCNAME=%%~nxI
if /I "%CCNAME%"=="cl" set CSTYLE=msvc
if /I "%CCNAME%"=="cl.exe" set CSTYLE=msvc

:cc_ready
if "%CSTYLE%"=="gnu" if "%AR%"=="" set AR=ar

:: --- 1. fetch luasocket (pinned release, skipped when already extracted) ---
if exist "%SRC%\luasocket.c" goto fetch_done
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BUILD%\luasocket-%LUASOCKET_VERSION%.tar.gz" (
    echo Fetching luasocket %LUASOCKET_VERSION%...
    where curl >nul 2>&1
    if errorlevel 1 (echo Error: need curl to download luasocket. & exit /b 1)
    curl -sL -o "%BUILD%\luasocket-%LUASOCKET_VERSION%.tar.gz" "%LUASOCKET_URL%"
    if errorlevel 1 (echo Error: could not download luasocket. & exit /b 1)
)
tar xzf "%BUILD%\luasocket-%LUASOCKET_VERSION%.tar.gz" -C "%BUILD%"
if errorlevel 1 (echo Error: could not extract luasocket archive. & exit /b 1)
:fetch_done

:: --- 2. compile the C cores against clx's Lua C API headers ---
echo Compiling luasocket C modules with %CC%...
if not exist "%BUILD%\obj" mkdir "%BUILD%\obj"
set SOCKET_OBJS=
for %%f in (luasocket auxiliar buffer compat except inet io options select tcp timeout udp wsocket mime) do (
    if "%CSTYLE%"=="msvc" (
        %CC% /nologo /O1 /GL /Gw /Oi /I"%CLX_INCLUDE%" /I"%SRC%" /c "%SRC%\%%f.c" /Fo"%BUILD%\obj\%%f.obj"
        if errorlevel 1 (echo Error: failed compiling %%f.c & exit /b 1)
        set SOCKET_OBJS=!SOCKET_OBJS! "%BUILD%\obj\%%f.obj"
    ) else (
        %CC% -c -Os -I"%CLX_INCLUDE%" -I"%SRC%" "%SRC%\%%f.c" -o "%BUILD%\obj\%%f.o"
        if errorlevel 1 (echo Error: failed compiling %%f.c & exit /b 1)
        set SOCKET_OBJS=!SOCKET_OBJS! "%BUILD%\obj\%%f.o"
    )
)

:: Archive names match the require names: socket.core -> socket.core.lib
if "%CSTYLE%"=="msvc" (
    lib /nologo /OUT:"%BUILD%\socket.core.lib" !SOCKET_OBJS!
    if errorlevel 1 (echo Error: could not create socket.core.lib & exit /b 1)
    lib /nologo /OUT:"%BUILD%\mime.core.lib" "%BUILD%\obj\mime.obj"
    if errorlevel 1 (echo Error: could not create mime.core.lib & exit /b 1)
) else (
    %AR% rcs "%BUILD%\socket.core.lib" !SOCKET_OBJS!
    if errorlevel 1 (echo Error: could not create socket.core.lib & exit /b 1)
    %AR% rcs "%BUILD%\mime.core.lib" "%BUILD%\obj\mime.o"
    if errorlevel 1 (echo Error: could not create mime.core.lib & exit /b 1)
)

:: --- 3. stage the Lua side; the directory layout gives the dotted require names ---
echo Staging Lua modules...
for %%f in (socket.lua ltn12.lua mime.lua) do (
    copy /Y "%SRC%\%%f" "%BUILD%\" >nul
    if errorlevel 1 (echo Error: could not stage %%f & exit /b 1)
)
if not exist "%BUILD%\socket" mkdir "%BUILD%\socket"
for %%f in (url.lua headers.lua http.lua tp.lua) do (
    copy /Y "%SRC%\%%f" "%BUILD%\socket\" >nul
    if errorlevel 1 (echo Error: could not stage %%f & exit /b 1)
)
copy /Y "..\..\benchmarks\dkjson.lua" "%BUILD%\" >nul
if errorlevel 1 (echo Error: could not stage dkjson.lua & exit /b 1)

:: --- 4. compile and link the example ---
:: luasocket needs the Winsock library; clx forwards compiler options to the link
echo Compiling weather...
set WS2_32=/DEFAULTLIB:ws2_32.lib
if not "%CSTYLE%"=="msvc" set WS2_32=-lws2_32
:: MSVC smallest-code flags (the /link marker keeps them on the compile side,
:: like build.sh passes -Oz/-flto=auto on POSIX)
set MSVC_SIZE=/Os /Oi /Zc:inline /link
if not "%CSTYLE%"=="msvc" set MSVC_SIZE=
pushd "%BUILD%"
"%CLX%" ../weather.lua socket.lua socket/url.lua socket/headers.lua socket/http.lua socket/tp.lua ltn12.lua mime.lua dkjson.lua --modules socket.core,mime.core %MSVC_SIZE% %WS2_32% -o weather.exe
set CLX_EXIT=!errorlevel!
popd
if not "!CLX_EXIT!"=="0" exit /b !CLX_EXIT!
echo Done. Run build\weather.exe ^(or build\weather.exe 48.85 2.35 Paris^)
exit /b 0
