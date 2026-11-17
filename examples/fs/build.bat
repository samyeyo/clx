@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

:: Configuration - same names and defaults as build.sh
if "%CLX_INCLUDE%"=="" set CLX_INCLUDE=..\..\include
set BUILD=build

:: Detect clx (in-tree build first, then an installed layout)
set CLX=..\..\build\clx.exe
if not exist "%CLX%" set CLX=..\..\bin\clx.exe
if not exist "%CLX%" (
    echo clx.exe not found.
    echo Please build clx first by running "build.bat" in the repository root.
    exit /b 1
)
for %%I in ("%CLX%") do set "CLX=%%~fI"
for %%I in ("%CLX_INCLUDE%") do set "CLX_INCLUDE=%%~fI"

if not exist "%CLX_INCLUDE%\clx.hpp" (
    echo Error: %CLX_INCLUDE%\clx.hpp not found ^(set CLX_INCLUDE to clx's include directory.^)
    exit /b 1
)

:: Detect C++ compiler style from the compiler clx itself drives (clx --cxx), so the module
:: archive matches the dialect clx links with. CXX overrides the pick.
set DIALECT=MSVC
for /f "tokens=1" %%I in ('%CLX% --cxx 2^>nul') do set DIALECT=%%I
set CSTYLE=gnu
if "%DIALECT%"=="MSVC" set CSTYLE=msvc
if "%DIALECT%"=="ClangCL" set CSTYLE=msvc

set WANT=g++
if "%CSTYLE%"=="msvc" set WANT=cl
if "%DIALECT%"=="ClangCL" set WANT=clang-cl
if "%CXX%"=="" set "CXX=%WANT%.exe"
for %%I in ("%CXX%") do set "CXXNAME=%%~nxI"
where %CXXNAME% >nul 2>&1
if errorlevel 1 (
    echo Error: %CXXNAME% not found in PATH.
    if "%CSTYLE%"=="msvc" echo Run this from an "x64 Native Tools Command Prompt".
    exit /b 1
)
if "%CSTYLE%"=="gnu" if "%AR%"=="" set AR=ar

if not exist "%BUILD%" mkdir "%BUILD%"

:: --- 1. the Lua side: an archive exporting luaopen_fs, which `--modules fs` scans for
:: ---    (clx looks for fs.lib under MSVC, fs.a elsewhere)
echo Compiling the fs module archive...
if "%CSTYLE%"=="msvc" (
    %CXX% /nologo /std:c++20 /MD /EHsc /utf-8 /O2 /I"%CLX_INCLUDE%" /c fs.cpp /Fo:"%BUILD%\fs.obj"
    if errorlevel 1 (echo Error: could not compile fs.cpp & exit /b 1)
    lib /nologo /OUT:"%BUILD%\fs.lib" "%BUILD%\fs.obj"
    if errorlevel 1 (echo Error: could not create fs.lib & exit /b 1)
) else (
    %CXX% -std=c++20 -O2 -I"%CLX_INCLUDE%" -c fs.cpp -o "%BUILD%\fs.o"
    if errorlevel 1 (echo Error: could not compile fs.cpp & exit /b 1)
    %AR% rcs "%BUILD%\fs.a" "%BUILD%\fs.o"
    if errorlevel 1 (echo Error: could not create fs.a & exit /b 1)
)

:: --- 2. compile demo.lua with the module archive in the working directory
echo Compiling demo.lua...
copy /Y demo.lua "%BUILD%\" >nul
if errorlevel 1 (echo Error: could not stage demo.lua & exit /b 1)
pushd "%BUILD%"
"%CLX%" demo.lua --minimal --modules fs,table -o demo.exe
set CLX_EXIT=!errorlevel!
popd
if not "!CLX_EXIT!"=="0" exit /b !CLX_EXIT!

echo Done. Run build\demo.exe ^(demo.lua with fs linked in^)
exit /b 0
