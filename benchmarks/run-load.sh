#!/bin/bash
# ┌─────────────────────────────────────────────┐
# │  clx — Lua to C++ Native Compiler           │
# │  benchmarks/run-load.sh · load() benchmarks │
# └─────────────────────────────────────────────┘
#
# Benchmarks each script by:
#   1. AOT — native clx compilation (--fast)
#   2. LOAD — compiled with --dynamic, loads the source via load()
#             and runs it through the embedded Lua VM
#
# Compares against stock Lua 5.5 and LuaJIT.
#
# Requires: lua, luajit, hyperfine, python3, awk
#
# Usage:
#   ./benchmarks/run-load.sh

set -euo pipefail

cd "$(dirname "$0")/.." || exit 1

TEST_DIR="benchmarks"
CLX_CMD="./build/clx"

## Default to --fast for benchmarking (clx defaults to --size)
## Override with CPPFLAGS="--size" to measure size-optimized performance
CPPFLAGS="${CPPFLAGS:---fast}"

TMPDIR="${TMPDIR:-/tmp}/clx_bench"
RESULTS_JSON="$TMPDIR/load_results.json"
LOADER_SRC="$TMPDIR/run_load_shim.lua"
LOADER_EXE="$TMPDIR/run_load_shim"
mkdir -p "$TMPDIR"

RUNS=10
WARMUP=3

for cmd in lua luajit awk hyperfine python3; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "Error: '$cmd' not found."
        exit 1
    fi
done

if [ ! -f "$CLX_CMD" ]; then
    echo "Error: CLX not built. Run ./build.sh first."
    exit 1
fi

#──────────────────────────────────────────────────────────────────────────────
# Build a generic load() shim used for every benchmark.
#   Usage: run_load_shim <script.lua>
# The shim sets up package.path so that require() calls inside the loaded
# chunk resolve from the benchmark directory (needed for canada.lua which
# does require("dkjson")).
#──────────────────────────────────────────────────────────────────────────────
cat > "$LOADER_SRC" << 'LUAEOF'
-- Loads and executes a Lua script via load().
-- Usage: run_load_shim <script.lua>
--   Prepends a package.path extension so that require() inside the
--   loaded chunk can find sibling .lua files (e.g. canada.lua requires
--   dkjson).  The preamble runs inside the Lua VM, not in AOT code,
--   so package is already fully initialized by the Lua VM.
local script_path = arg[1]
if not script_path then
    io.stderr:write("Usage: run_load_shim <script.lua>\n")
    os.exit(1)
end

-- Derive the script's directory for the preamble below, normalizing
-- backslashes to forward slashes so it is safe inside a Lua string literal.
local script_dir = script_path:match("^(.*)[/\\]") or "./"
script_dir = script_dir:gsub("\\", "/")

local f = io.open(script_path, "rb")
if not f then
    io.stderr:write("Error: cannot open ", script_path, "\n")
    os.exit(1)
end
local content = f:read("*a")
f:close()

if not content or content == "" then
    io.stderr:write("Error: empty file ", script_path, "\n")
    os.exit(1)
end

-- Prepend a package.path extension so require() in the loaded chunk
-- (which runs inside the VM) can find sibling modules.  The directory is
-- baked in as a literal so package.path gets the real path at runtime.
local preamble = 'package.path = package.path .. ";" .. "' .. script_dir .. '?.lua"\n'
local load_content = preamble .. content

local chunk, err = load(load_content, script_path, "t")
if not chunk then
    io.stderr:write("load error: ", err, "\n")
    os.exit(1)
end

local ok, run_err = pcall(chunk)
if not ok then
    io.stderr:write("runtime error: ", run_err, "\n")
    os.exit(1)
end
LUAEOF

echo "Building load() shim..."
if ! $CLX_CMD "$LOADER_SRC" --output "$LOADER_EXE" --dynamic $CPPFLAGS 2>/dev/null; then
    echo "Error: Failed to compile load() shim"
    exit 1
fi

# CPU pinning
CPU_PIN=""
if command -v taskset &>/dev/null; then
    CPU_PIN="taskset -c 0"
fi

echo ""
echo "Benchmarking via load() with hyperfine ($RUNS runs, $WARMUP warmup)..."
echo "========================================================================================================="
printf "%-22s | %-15s | %-18s | %-18s | %-18s\n" \
    "Script" "lua 5.5" "LuaJIT" "clx AOT" "clx load()"
echo "========================================================================================================="

FOUND_FILES=0

for file in "$TEST_DIR"/*.lua; do
    [ -e "$file" ] || continue
    basename=$(basename "$file" .lua)

    # Skip *_luajit.lua — they are only run with luajit
    case "$basename" in *_luajit | dkjson | run | run-hyperfine | run-load | warmup) continue ;; esac

    luajit_file="$TEST_DIR/${basename}_luajit.lua"
    [ -f "$luajit_file" ] || luajit_file="$file"

    FOUND_FILES=1
    clx_exe="$TMPDIR/$basename"

    rm -f "$clx_exe"

    # Multi-module benchmarks: pass extra sources to clx AOT compiler
    extra=""
    case "$basename" in
        canada) extra="$TEST_DIR/dkjson.lua" ;;
    esac

    if ! $CLX_CMD "$file" $extra --output "$clx_exe" $CPPFLAGS 2>/dev/null || [ ! -x "$clx_exe" ]; then
        printf "%-22s | %-15s | %-18s | %-18s | %-18s\n" \
            "$basename.lua" "CLX COMPILE FAIL" "-" "-" "-"
        continue
    fi

    bname="$(basename "$file")"
    lbname="$(basename "$luajit_file")"

    hyperfine --ignore-failure --warmup "$WARMUP" --runs "$RUNS" \
        --export-json "$RESULTS_JSON" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec lua \"$bname\"'" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec luajit \"$lbname\"'" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec \"$clx_exe\"'" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec \"$LOADER_EXE\" \"$bname\"'" \
        >/dev/null 2>&1

    # Extract mean times from JSON via python3.
    # Results are returned in the order: lua, luajit, clx AOT, clx load().
    read -r avg_lua avg_luajit avg_clx avg_load <<< $(python3 -c "
import json,sys
with open('$RESULTS_JSON') as f:
    data = json.load(f)
vals = []
for r in data['results']:
    v = r.get('mean')
    vals.append('{:.3f}'.format(v) if v is not None else 'nan')
while len(vals) < 4: vals.append('nan')
print('{} {} {} {}'.format(vals[0], vals[1], vals[2], vals[3]))
")

    # Speedups relative to stock Lua
    sp_luajit=$(awk "BEGIN { v=\"$avg_luajit\"; if(v==\"nan\"){printf \"%-6s\",\"FAIL\";exit} printf \"%.2fx\", $avg_lua / v }")
    sp_clx=$(awk    "BEGIN { v=\"$avg_clx\";    if(v==\"nan\"){printf \"%-6s\",\"FAIL\";exit} printf \"%.2fx\", $avg_lua / v }")
    sp_load=$(awk   "BEGIN { v=\"$avg_load\";   if(v==\"nan\"){printf \"%-6s\",\"FAIL\";exit} printf \"%.2fx\", $avg_lua / v }")

    fmt() { awk "BEGIN { v=\"$1\"; if(v==\"nan\"){printf \"%-5s\",\"FAIL\";exit} printf \"%.3fs\", v }"; }
    printf "%-22s | %-5s (1.00x)  | %-5s (%-6s)    | %-5s (%-6s)    | %-5s (%-6s)\n" \
        "$basename.lua" \
        "$(fmt "$avg_lua")" \
        "$(fmt "$avg_luajit")" "$sp_luajit" \
        "$(fmt "$avg_clx")" "$sp_clx" \
        "$(fmt "$avg_load")" "$sp_load"

    rm -f "$clx_exe"
done

rm -f "$LOADER_SRC" "$LOADER_EXE" "$RESULTS_JSON"

if [ $FOUND_FILES -eq 0 ]; then
    echo "No .lua scripts found in $TEST_DIR/."
fi

echo "================================================================================================"
echo "Benchmarking complete."
