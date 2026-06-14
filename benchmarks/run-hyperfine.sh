#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.." || exit 1

TEST_DIR="benchmarks"
CLX_CMD="./build/clx"

## Default to --fast for benchmarking (clx defaults to --size)
## Override with CPPFLAGS="--size" to measure size-optimized performance
CPPFLAGS="${CPPFLAGS:---fast}"

TMPDIR="${TMPDIR:-/tmp}/clx_bench"
RESULTS_JSON="$TMPDIR/hyperfine_results.json"
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

# Pin to a single CPU
CPU_PIN=""
if command -v taskset &>/dev/null; then
    CPU_PIN="taskset -c 0"
fi

echo "Benchmarking with hyperfine ($RUNS runs, $WARMUP warmup)..."
echo "========================================================================================"
printf "%-22s | %-15s | %-18s | %-18s\n" "Script" "lua 5.5" "LuaJIT" "clx"
echo "========================================================================================"

FOUND_FILES=0

for file in "$TEST_DIR"/*.lua; do
    [ -e "$file" ] || continue
    basename=$(basename "$file" .lua)

    # Skip *_luajit.lua — they are only run with luajit
    case "$basename" in *_luajit | dkjson) continue ;; esac

    luajit_file="$TEST_DIR/${basename}_luajit.lua"
    [ -f "$luajit_file" ] || luajit_file="$file"

    FOUND_FILES=1
    clx_exe="$TMPDIR/$basename"

    rm -f "$clx_exe"

    # Multi-module benchmarks
    extra=""
    case "$basename" in
        canada) extra="$TEST_DIR/dkjson.lua" ;;
    esac

    if ! $CLX_CMD "$file" $extra --output "$clx_exe" $CPPFLAGS 2>/dev/null || [ ! -x "$clx_exe" ]; then
        printf "%-22s | %-15s | %-18s | %-18s\n" "$basename.lua" "CLX COMPILE FAIL" "-" "-"
        continue
    fi

    # Run hyperfine on all three engines. Use --ignore-failure so a single
    # engine fail (e.g. luajit on unsupported Lua features) doesn't kill the rest.
    bname="$(basename "$file")"
    lbname="$(basename "$luajit_file")"
    hyperfine --ignore-failure --warmup "$WARMUP" --runs "$RUNS" \
        --export-json "$RESULTS_JSON" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec lua \"$bname\"'" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec luajit \"$lbname\"'" \
        "$CPU_PIN sh -c 'cd \"$TEST_DIR\" && exec \"$clx_exe\"'" \
        >/dev/null 2>&1

    # Extract mean times from JSON via python3.
    # When a benchmark fails (--ignore-failure) its 'mean' is null in JSON;
    # we output 'nan' and display "FAIL" in the table.
    # Results are returned in the order lua, luajit, clx.
    read -r avg_lua avg_luajit avg_clx <<< $(python3 -c "
import json,sys
with open('$RESULTS_JSON') as f:
    data = json.load(f)
vals = []
for r in data['results']:
    v = r.get('mean')
    vals.append('{:.3f}'.format(v) if v is not None else 'nan')
while len(vals) < 3: vals.append('nan')
print('{} {} {}'.format(vals[0], vals[1], vals[2]))
")

    # Speedups
    sp_luajit=$(awk "BEGIN { v=\"$avg_luajit\"; if(v==\"nan\"){printf \"%-6s\",\"FAIL\";exit} printf \"%.2fx\", $avg_lua / v }")
    sp_clx=$(awk "BEGIN { v=\"$avg_clx\"; if(v==\"nan\"){printf \"%-6s\",\"FAIL\";exit} printf \"%.2fx\", $avg_lua / v }")

    fmt() { awk "BEGIN { v=\"$1\"; if(v==\"nan\"){printf \"%-5s\",\"FAIL\";exit} printf \"%.3fs\", v }"; }
    printf "%-22s | %-5s (1.00x)  | %-5s (%-6s)    | %-5s (%-6s)\n" \
        "$basename.lua" \
        "$(fmt "$avg_lua")" \
        "$(fmt "$avg_luajit")" "$sp_luajit" \
        "$(fmt "$avg_clx")" "$sp_clx"

    rm -f "$clx_exe"
done

if [ $FOUND_FILES -eq 0 ]; then
    echo "No .lua scripts found in $TEST_DIR/."
fi

echo "========================================================================================"
echo "Benchmarking complete."
