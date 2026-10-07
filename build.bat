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
if /I "%~1"=="clang-cl" set USE_CLANG_CL=1
if /I "%~1"=="clangcl"  set USE_CLANG_CL=1
if /I "%~1"=="mingw"    set USE_MINGW=1
shift
goto parse
:done_parse

set GEN=NMake Makefiles
set TOOLCHAIN_ARGS=
if defined USE_MINGW (
    set "GEN=MinGW Makefiles"
    set "TOOLCHAIN_ARGS=-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_ASM_COMPILER=gcc"
    set "GCC_AR="
    set "GCC_RANLIB="
    for /f "delims=" %%A in ('where gcc-ar 2^>nul') do if not defined GCC_AR set "GCC_AR=%%A"
    for /f "delims=" %%A in ('where gcc-ranlib 2^>nul') do if not defined GCC_RANLIB set "GCC_RANLIB=%%A"
    if defined GCC_AR set "TOOLCHAIN_ARGS=!TOOLCHAIN_ARGS! -DCMAKE_AR=!GCC_AR!"
    if defined GCC_RANLIB set "TOOLCHAIN_ARGS=!TOOLCHAIN_ARGS! -DCMAKE_RANLIB=!GCC_RANLIB!"
) else if defined USE_CLANG_CL (
    set "TOOLCHAIN_ARGS=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_ASM_COMPILER=clang-cl"
)

set ARCH_PRESERVE=
if exist build\CMakeCache.txt (
    for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /c:"CLX_ARCH:STRING=" build\CMakeCache.txt 2^>nul`) do set "ARCH_PRESERVE=-D%%a=%%b"
)

rem CMake caches the generator and compiler, so wipe build/ when either changed (cl <-> clang-cl <-> mingw)
if exist build\CMakeCache.txt (
    findstr /i /c:"MinGW Makefiles" build\CMakeCache.txt >nul 2>&1 && set "CACHED_MINGW=1"
    findstr /i /c:"clang-cl" build\CMakeCache.txt >nul 2>&1 && set "CACHED_CLANG=1"
    if defined USE_MINGW if not defined CACHED_MINGW rmdir /s /q build 2>nul
    if defined USE_CLANG_CL if not defined CACHED_CLANG rmdir /s /q build 2>nul
    if not defined USE_MINGW if defined CACHED_MINGW rmdir /s /q build 2>nul
    if not defined USE_MINGW if not defined USE_CLANG_CL if defined CACHED_CLANG rmdir /s /q build 2>nul
)

set ARCH_ARGS=
for /f "tokens=1,* delims==" %%a in ('set CLX_ 2^>nul') do (
    set ARCH_ARGS=!ARCH_ARGS! -D%%a=%%b
)

cmake -S . -B build -G "%GEN%" -D CMAKE_BUILD_TYPE=%BUILD_TYPE% !ARCH_PRESERVE! !TOOLCHAIN_ARGS! !ARCH_ARGS!
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
set "MANIFEST=build\install_manifest.txt"
if not exist "!MANIFEST!" (
    echo Cannot find install manifest. Is the project installed?
    exit /b 1
)
echo Uninstalling...
for /f "usebackq tokens=*" %%f in ("!MANIFEST!") do (
    if exist "%%f" del /f /q "%%f"
)
echo Uninstallation complete.
exit /b 0