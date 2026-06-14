@echo off
setlocal
cd /d "%~dp0"

:: Auto-detect compiler: prefer clang++, then cl.exe, then g++
set CXX=
where clang++ >nul 2>&1
if not errorlevel 1 set CXX=clang++
if "%CXX%"=="" (
    where cl.exe >nul 2>&1
    if not errorlevel 1 set CXX=cl.exe
)
if "%CXX%"=="" (
    where g++ >nul 2>&1
    if not errorlevel 1 set CXX=g++
)
if "%CXX%"=="" (
    echo Error: No supported compiler found. Install clang++, MSVC, or g++.
    exit /b 1
)

echo Compiling sokol_clx module with %CXX%...

if "%CXX%"=="cl.exe" (
    cl.exe /nologo /std:c++20 /MD /O1 /GL /EHsc /Gy /I..\..\include /I.\sokol /c sokol_clx.cpp /Fosokol_clx.obj
) else (
    %CXX% -std=c++20 -Oz -fvisibility=hidden -ffunction-sections -fdata-sections -I..\..\include -I.\sokol -c sokol_clx.cpp -o sokol_clx.obj
)

lib /OUT:sokol_clx.lib sokol_clx.obj >nul 2>&1
del sokol_clx.obj
echo Created sokol_clx.lib
