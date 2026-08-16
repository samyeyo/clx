@echo off
setlocal enabledelayedexpansion

:: Locate project root
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT_DIR=%%~fI"

:: Find clx compiler
if exist "%ROOT_DIR%\build\bin\Release\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\bin\Release\clx.exe"
) else if exist "%ROOT_DIR%\build\bin\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\bin\clx.exe"
) else if exist "%ROOT_DIR%\build\clx.exe" (
    set "COMPILER=%ROOT_DIR%\build\clx.exe"
) else if exist "%ROOT_DIR%\bin\clx.exe" (
    set "COMPILER=%ROOT_DIR%\bin\clx.exe"
) else (
    echo Error: clx.exe not found. Run build.bat first.
    exit /b 1
)

echo Using compiler: %COMPILER%
echo.

cd /d "%SCRIPT_DIR%"

set PASS=0
set FAIL=0

:: Walk through subdirectories
for %%D in (conformance regression stress compiler_killers edge_cases) do (
    if exist "%%D" (
        for /f "delims=" %%F in ('dir /b "%%D\*.lua" 2^>nul ^| findstr /v /i "^package\.lua$ ^mymod\.lua$ ^test_native_mod\.lua$ ^load\.lua$"') do (
            set "name=%%~nF"
            set "prefix=%%D_!name!"

            "%COMPILER%" "%%D\%%F" --output "!prefix!" > "!prefix!_compile.log" 2>&1

            if exist "!prefix!.exe" (
                echo --- %%D\%%F ---
                "!prefix!.exe" > "!prefix!_output.log" 2>&1
                set "EXIT_CODE=!errorlevel!"
                type "!prefix!_output.log"

                if !EXIT_CODE! neq 0 (
                    echo [FAIL] %%D/!name! -- runtime exit code !EXIT_CODE!
                    set /a FAIL+=1
                ) else (
                    findstr /c:"[FAIL]" "!prefix!_output.log" >nul 2>&1
                    if !errorlevel! equ 0 (
                        echo [FAIL] %%D/!name!
                        set /a FAIL+=1
                    ) else (
                        echo [PASS] %%D/!name!
                        set /a PASS+=1
                    )
                )
                del /f /q "!prefix!.exe" >nul 2>&1
                del /f /q "!prefix!_compile.log" >nul 2>&1
                del /f /q "!prefix!_output.log" >nul 2>&1
            ) else (
                echo [FAIL] %%D/!name! -- compilation failed
                type "!prefix!_compile.log"
                del /f /q "!prefix!_compile.log" >nul 2>&1
                set /a FAIL+=1
            )
            echo.
        )
    )
)

:: ----------------------------------------------------------------------
:: Real-world compatibility tests (each dir has a test.lua entry point;
:: compile all of the dir's .lua files together)
:: ----------------------------------------------------------------------
if exist "realworld" (
    for /f "delims=" %%D in ('dir /b /ad "realworld" 2^>nul') do (
        set "rwtest=%SCRIPT_DIR%\realworld\%%D\test.lua"
        if exist "!rwtest!" (
            set "name=%%D"
            set "bin=%SCRIPT_DIR%\realworld_!name!"

            REM Build the list of top-level .lua sources in the directory
            set "SRCS="
            for /f "delims=" %%F in ('dir /b "realworld\!name!\*.lua" 2^>nul') do (
                set "SRCS=!SRCS! "realworld\!name!\%%F""
            )

            echo --- realworld\%%D ---
            "%COMPILER%" !SRCS! --output "!bin!" > "!bin!_compile.log" 2>&1

            if exist "!bin!.exe" (
                "!bin!.exe" > "!bin!_output.log" 2>&1
                set "EXIT_CODE=!errorlevel!"
                type "!bin!_output.log"

                if !EXIT_CODE! neq 0 (
                    echo [FAIL] realworld/!name! -- runtime exit code !EXIT_CODE!
                    set /a FAIL+=1
                ) else (
                    findstr /c:"[FAIL]" "!bin!_output.log" >nul 2>&1
                    if !errorlevel! equ 0 (
                        echo [FAIL] realworld/!name!
                        set /a FAIL+=1
                    ) else (
                        echo [PASS] realworld/!name!
                        set /a PASS+=1
                    )
                )
                del /f /q "!bin!.exe" >nul 2>&1
                del /f /q "!bin!_compile.log" >nul 2>&1
                del /f /q "!bin!_output.log" >nul 2>&1
            ) else (
                echo [FAIL] realworld/!name! -- compilation failed
                type "!bin!_compile.log"
                del /f /q "!bin!_compile.log" >nul 2>&1
                set /a FAIL+=1
            )
            echo.
        )
    )
)

:: ----------------------------------------------------------------------
:: Native module test (--modules)
:: ----------------------------------------------------------------------
echo.
echo --- native_mod (C++ --modules) ---

set "NATIVE_SRC=%SCRIPT_DIR%\conformance\native_mod.cpp"
set "NATIVE_TEST=%SCRIPT_DIR%\conformance\test_native_mod.lua"
set "NATIVE_BIN=%SCRIPT_DIR%\native_mod_test.exe"

if exist "%NATIVE_SRC%" if exist "%NATIVE_TEST%" (
    set "NATIVE_OK="
    :: Try MSVC first, then MinGW g++
    where cl.exe >nul 2>&1
    if !errorlevel! equ 0 (
        cl /c /std:c++20 /MD /EHsc /I"%ROOT_DIR%\include" "%NATIVE_SRC%" /Fo"%SCRIPT_DIR%\native_mod.obj" 2>&1
        if exist "%SCRIPT_DIR%\native_mod.obj" (
            lib /OUT:"%SCRIPT_DIR%\native_mod.lib" "%SCRIPT_DIR%\native_mod.obj" >nul 2>&1
            "%COMPILER%" "%NATIVE_TEST%" --modules native_mod --output "%NATIVE_BIN%" 2>&1
            if exist "%NATIVE_BIN%" (
                set "NATIVE_OK=1"
            ) else (
                echo [FAIL] native_mod -- linking with clx compiler failed
            )
        ) else (
            echo [FAIL] native_mod -- MSVC compilation of native_mod.cpp failed
        )
        del /f /q "%SCRIPT_DIR%\native_mod.obj" "%SCRIPT_DIR%\native_mod.lib" >nul 2>&1
    ) else (
        g++ -c -std=c++20 -I"%ROOT_DIR%\include" "%NATIVE_SRC%" -o "%SCRIPT_DIR%\native_mod.o" 2>&1
        if exist "%SCRIPT_DIR%\native_mod.o" (
            ar rcs "%SCRIPT_DIR%\native_mod.a" "%SCRIPT_DIR%\native_mod.o" >nul 2>&1
            "%COMPILER%" "%NATIVE_TEST%" --modules native_mod --output "%NATIVE_BIN%" 2>&1
            if exist "%NATIVE_BIN%" (
                set "NATIVE_OK=1"
            ) else (
                echo [FAIL] native_mod -- linking with clx compiler failed
            )
        ) else (
            echo [FAIL] native_mod -- g++ compilation of native_mod.cpp failed
        )
        del /f /q "%SCRIPT_DIR%\native_mod.o" "%SCRIPT_DIR%\native_mod.a" >nul 2>&1
    )
    if defined NATIVE_OK (
        "%NATIVE_BIN%" > "%SCRIPT_DIR%\native_mod_out.log" 2>&1
        set "NATIVE_EXIT=!errorlevel!"
        type "%SCRIPT_DIR%\native_mod_out.log"
        if !NATIVE_EXIT! neq 0 (
            echo [FAIL] native_mod -- runtime exit code !NATIVE_EXIT!
            set /a FAIL+=1
        ) else (
            findstr /c:"[FAIL]" "%SCRIPT_DIR%\native_mod_out.log" >nul 2>&1
            if !errorlevel! equ 0 ( echo [FAIL] native_mod & set /a FAIL+=1 ) else ( echo [PASS] native_mod & set /a PASS+=1 )
        )
        del /f /q "%NATIVE_BIN%" >nul 2>&1
        del /f /q "%SCRIPT_DIR%\native_mod_out.log" >nul 2>&1
    ) else (
        echo [FAIL] native_mod
        set /a FAIL+=1
    )
)

:: ----------------------------------------------------------------------
:: Multi-module test (package.lua + mymod.lua)
:: ----------------------------------------------------------------------
echo.
echo --- package.lua + mymod.lua ---

set "MULTI_BIN=%SCRIPT_DIR%\package_mymod_test.exe"

"%COMPILER%" "%SCRIPT_DIR%\conformance\package.lua" "%SCRIPT_DIR%\conformance\mymod.lua" --output "%MULTI_BIN%" > "%SCRIPT_DIR%\package_compile.log" 2>&1
if exist "%MULTI_BIN%" (
    "%MULTI_BIN%" > "%SCRIPT_DIR%\package_output.log" 2>&1
    set "MULTI_EXIT=!errorlevel!"
    type "%SCRIPT_DIR%\package_output.log"
    if !MULTI_EXIT! neq 0 (
        echo [FAIL] package+mymod -- runtime exit code !MULTI_EXIT!
        set /a FAIL+=1
    ) else (
        findstr /c:"[FAIL]" "%SCRIPT_DIR%\package_output.log" >nul 2>&1
        if !errorlevel! equ 0 ( echo [FAIL] package+mymod & set /a FAIL+=1 ) else ( echo [PASS] package+mymod & set /a PASS+=1 )
    )
    del /f /q "%MULTI_BIN%" >nul 2>&1
    del /f /q "%SCRIPT_DIR%\package_output.log" >nul 2>&1
) else (
    echo [FAIL] package+mymod -- compilation failed
    type "%SCRIPT_DIR%\package_compile.log"
    set /a FAIL+=1
)
del /f /q "%SCRIPT_DIR%\package_compile.log" >nul 2>&1

echo.
echo Results: %PASS% passed, %FAIL% failed.
if %FAIL% gtr 0 exit /b 1
exit /b 0
