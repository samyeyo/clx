@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

:: Detect clx
set CLX=..\..\bin\clx.exe
if not exist "%CLX%" (
    echo clx.exe not found
    echo Please build clx first by running "build.bat install" in the root directory.
    exit /b 1
)

:: Ask clx which compiler drives the link: MSVC / ClangCL get MSVC flags, GNU gets GCC flags
set DIALECT=MSVC
for /f "tokens=1,*" %%a in ('%CLX% --cxx 2^>nul') do set DIALECT=%%a
set MODULE=sokol_clx.lib
set "GUIFLAGS=/link /subsystem:windows /entry:mainCRTStartup"
if not "%DIALECT%"=="MSVC" if not "%DIALECT%"=="ClangCL" (
    set MODULE=sokol_clx.a
    set "GUIFLAGS=-mwindows -luser32 -lgdi32 -lopengl32"
)

:: Build sokol module if needed
if not exist "..\sokol\%MODULE%" (
    echo Building sokol_clx module...
    pushd ..\sokol
    call build.bat
    popd
)
if not exist "..\sokol\%MODULE%" (
    echo Error: ..\sokol\%MODULE% was not built
    exit /b 1
)

:: Copy module to current dir for clx --modules to find
copy /Y "..\sokol\%MODULE%" "%MODULE%" >nul
if not exist "%MODULE%" (
    echo Error: could not copy %MODULE%
    exit /b 1
)

:: Compile pong
%CLX% pong.lua --modules sokol_clx --output pong.exe %GUIFLAGS%
set ERR=!errorlevel!
if exist "%MODULE%" del "%MODULE%"
if not "!ERR!"=="0" exit /b !ERR!
echo Done. Run pong.exe to play.
