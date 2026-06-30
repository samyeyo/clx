#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR=$(dirname "$SCRIPT_DIR")

if [ -f "$ROOT_DIR/build/clx" ]; then
    COMPILER="$ROOT_DIR/build/clx"
elif [ -f "$ROOT_DIR/build/bin/clx" ]; then
    COMPILER="$ROOT_DIR/build/bin/clx"
else
    COMPILER="/usr/bin/clx"
fi

cd "$SCRIPT_DIR" || exit

PASS=0
FAIL=0

# Lua tests via clx compiler — walk each subdirectory
for dir in conformance regression stress edge_cases; do
    [ -d "$dir" ] || continue
    for file in "$dir"/*.lua; do
        [ -f "$file" ] || continue
        name=$(basename "$file" .lua)
        # Skip files handled by dedicated tests below
        case "$file" in
            conformance/package.lua|conformance/mymod.lua|conformance/test_native_mod.lua) continue ;;
        esac
        bin="$SCRIPT_DIR/${dir}_${name}"
        compile_log="$bin.compile.log"
        output_log="$bin.output.log"

        "$COMPILER" "$file" --output "$bin" --debug > "$compile_log" 2>&1
        "$COMPILER" "$file" --output "$bin" --cpp

        if [ -f "$bin" ]; then
            "$bin" > "$output_log" 2>&1
            exit_code=$?
            cat "$output_log"

            if grep -q "\[FAIL\]" "$output_log" || [ "$exit_code" -ne "0" ]; then
                echo "[FAIL] $dir/$name"
                FAIL=$((FAIL + 1))
            else
                echo "[PASS] $dir/$name"
                PASS=$((PASS + 1))
            fi
            # rm -f "$compile_log" "$output_log"
            # rm -f "$bin" "$compile_log" "$output_log"
        else
            echo "[FAIL] $dir/$name -- compilation failed"
            "$COMPILER" "$file" --output "$bin" --cpp
            echo "compile log:"
            cat "$compile_log"
            FAIL=$((FAIL + 1))
            rm -f "$compile_log"
        fi
    done
done

# Native module test (--modules)
NATIVE_SRC="$SCRIPT_DIR/conformance/native_mod.cpp"
NATIVE_TEST_LUA="$SCRIPT_DIR/conformance/test_native_mod.lua"
NATIVE_BIN="$SCRIPT_DIR/native_mod_test"
if [ -f "$NATIVE_SRC" ] && [ -f "$NATIVE_TEST_LUA" ]; then
    g++ -c -std=c++20 -fPIC -I "$ROOT_DIR/include" -o "$SCRIPT_DIR/native_mod.o" "$NATIVE_SRC" 2>/dev/null
    if [ -f "$SCRIPT_DIR/native_mod.o" ]; then
        NATIVE_A="$SCRIPT_DIR/native_mod.a"
        ar rcs "$NATIVE_A" "$SCRIPT_DIR/native_mod.o" 2>/dev/null

        NATIVE_OUT="$SCRIPT_DIR/native_mod_out.log"
        "$COMPILER" "$NATIVE_TEST_LUA" --modules native_mod --output "$NATIVE_BIN" 2>/dev/null
        if [ -f "$NATIVE_BIN" ]; then
            "$NATIVE_BIN" > "$NATIVE_OUT" 2>&1
            exit_code=$?
            cat "$NATIVE_OUT"
            if grep -q "\[FAIL\]" "$NATIVE_OUT" || [ "$exit_code" -ne "0" ]; then
                echo "[FAIL] native_mod"
                FAIL=$((FAIL + 1))
            else
                echo "[PASS] native_mod"
                PASS=$((PASS + 1))
            fi
            rm -f "$NATIVE_BIN" "$NATIVE_OUT"
        fi

        rm -f "$NATIVE_A" "$SCRIPT_DIR/native_mod.o"
    fi
fi

# Multi-module static test (package.lua + mymod.lua)
echo ""
echo "--- package.lua + mymod.lua ---"
PKG_BIN="$SCRIPT_DIR/package_mymod_test"
"$COMPILER" "$SCRIPT_DIR/conformance/package.lua" "$SCRIPT_DIR/conformance/mymod.lua" --output "$PKG_BIN" 2>"$SCRIPT_DIR/package_compile.log"
if [ -f "$PKG_BIN" ]; then
    "$PKG_BIN" > "$SCRIPT_DIR/package_output.log" 2>&1
    exit_code=$?
    cat "$SCRIPT_DIR/package_output.log"
    if grep -q "\[FAIL\]" "$SCRIPT_DIR/package_output.log" || [ "$exit_code" -ne "0" ]; then
        echo "[FAIL] package+mymod"
        FAIL=$((FAIL + 1))
    else
        echo "[PASS] package+mymod"
        PASS=$((PASS + 1))
    fi
    rm -f "$PKG_BIN"
else
    echo "[FAIL] package+mymod -- compilation failed"
    cat "$SCRIPT_DIR/package_compile.log"
    FAIL=$((FAIL + 1))
fi
rm -f "$SCRIPT_DIR/package_compile.log" "$SCRIPT_DIR/package_output.log"

echo ""
echo "Results: $PASS passed, $FAIL failed."
if [ "$FAIL" -gt 0 ]; then exit 1; fi
