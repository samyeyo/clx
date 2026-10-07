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
if not exist "%CLX%" set CLX=..\..\build\clx.exe
if not exist "%CLX%" (
    echo clx.exe not found.
    echo Please build clx first by running "build.bat" in the repository root.
    exit /b 1
)
for %%I in ("%CLX%") do set "CLX=%%~fI"

if not exist "%CLX_INCLUDE%\lua.h" (
    echo Error: %CLX_INCLUDE%\lua.h not found ^(set CLX_INCLUDE to clx's include directory.^)
    exit /b 1
)

:: Detect C compiler style from the compiler clx itself drives (clx --cxx).
:: No goto anywhere below: cmd's forward label scan is byte-layout fragile.
set DIALECT=MSVC
for /f "tokens=1" %%I in ('%CLX% --cxx 2^>nul') do set DIALECT=%%I
set CSTYLE=gnu
if "%DIALECT%"=="MSVC" set CSTYLE=msvc
if "%DIALECT%"=="ClangCL" set CSTYLE=msvc
if not "%CC%"=="" (
    for %%I in ("%CC%") do set CCNAME=%%~nxI
    set CSTYLE=gnu
    if /I "!CCNAME!"=="cl" set CSTYLE=msvc
    if /I "!CCNAME!"=="cl.exe" set CSTYLE=msvc
) else if "!CSTYLE!"=="msvc" (
    where cl.exe >nul 2>&1
    if errorlevel 1 (
        echo Error: cl.exe not found. Run this from an "x64 Native Tools Command Prompt".
        exit /b 1
    )
    set "CC=cl.exe"
) else (
    set "CC="
    where gcc >nul 2>&1
    if not errorlevel 1 set "CC=gcc"
    if "!CC!"=="" (
        where clang >nul 2>&1
        if not errorlevel 1 set "CC=clang"
    )
    if "!CC!"=="" (
        echo Error: no C compiler found.
        echo Install gcc or clang, or run from an "x64 Native Tools Command Prompt" ^(cl.exe^).
        exit /b 1
    )
)
if "%CSTYLE%"=="gnu" if "%AR%"=="" set AR=ar

:: --- 1. fetch luasocket (pinned release, skipped when already extracted) ---
if not exist "%SRC%\luasocket.c" (
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
)

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
