@echo off
setlocal
cd /d "%~dp0"

:: Build sokol module if needed
if not exist "..\sokol\sokol_clx.lib" (
    echo Building sokol_clx module...
    pushd ..\sokol
    call build.bat
    popd
)

:: Copy module to current dir for clx --modules to find
copy /Y ..\sokol\sokol_clx.lib sokol_clx.lib >nul

:: Detect clx
if not exist "..\..\bin\clx.exe" (
    echo clx.exe not found
    echo Please build clx first by running "build.bat install" in the root directory.
)

:: Compile mandelbrot
..\..\bin\clx.exe mandelbrot.lua --modules sokol_clx --output mandelbrot.exe /link /subsystem:windows /entry:mainCRTStartup

del sokol_clx.lib
echo Done. Run mandelbrot.exe to explore.
