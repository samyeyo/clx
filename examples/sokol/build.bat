@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

:: Build sokol module with the same compiler clx links with (clx --cxx -> "NAME (path)")
:: No goto anywhere: cmd's forward label scan is byte-layout fragile.
set CLX=..\..\bin\clx.exe
if not exist "%CLX%" set CLX=..\..\build\clx.exe
if not exist "%CLX%" (
    echo Error: clx not found. Build clx first ^(build.bat in the repository root^).
    exit /b 1
)
set DIALECT=MSVC
set CXXPATH=g++
for /f "tokens=1,*" %%a in ('%CLX% --cxx 2^>nul') do (
    set DIALECT=%%a
    set CXXRAW=%%b
)
if defined CXXRAW (
    set CXXPATH=!CXXRAW:~1!
    if "!CXXPATH:~-1!"==")" set CXXPATH=!CXXPATH:~0,-1!
)
set ISMSVC=0
if "%DIALECT%"=="MSVC" set ISMSVC=1
if "%DIALECT%"=="ClangCL" set ISMSVC=1

echo Compiling sokol_clx module with %DIALECT%...

if "!ISMSVC!"=="1" (
    cl.exe /nologo /std:c++20 /MD /O1 /GL /EHsc /Gy /I..\..\include /I.\sokol /c sokol_clx.cpp /Fosokol_clx.obj
    if errorlevel 1 (
        echo Error: compiling sokol_clx.cpp failed
        exit /b 1
    )
    lib /OUT:sokol_clx.lib sokol_clx.obj
    if not exist sokol_clx.lib (
        echo Error: could not create sokol_clx.lib
        exit /b 1
    )
    del sokol_clx.obj
    echo Created sokol_clx.lib
) else (
    "%CXXPATH%" -std=c++20 -Oz -fvisibility=hidden -ffunction-sections -fdata-sections -I..\..\include -I.\sokol -c sokol_clx.cpp -o sokol_clx.obj
    if errorlevel 1 (
        echo Error: compiling sokol_clx.cpp failed
        exit /b 1
    )
    where ar >nul 2>&1
    if errorlevel 1 (
        echo Error: ar not found. Add your MinGW toolchain's bin directory to PATH.
        exit /b 1
    )
    ar rcs sokol_clx.a sokol_clx.obj
    if not exist sokol_clx.a (
        echo Error: could not create sokol_clx.a
        exit /b 1
    )
    del sokol_clx.obj
    echo Created sokol_clx.a
)
exit /b 0
