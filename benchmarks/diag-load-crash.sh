#!/bin/bash
# ─────────────────────────────────────────────
# │  diag-load-crash.sh · capture load() crash │
# ─────────────────────────────────────────────
# Runs a benchmark through the load() shim in a tight loop until it
# fails, then re-runs the failing case under gdb to capture a
# backtrace. Use this to diagnose intermittent run-load.sh failures.
#
# Usage: ./benchmarks/diag-load-crash.sh [benchmark.lua ...] [iterations]
#   benchmark.lua  default: canada.lua sieve.lua spectralnorm.lua
#   iterations     default: 200
set -u
cd "$(dirname "$0")/.." || exit 1

ITERS="200"
BENCHS=()
for a in "$@"; do
    case "$a" in
        *[!0-9]*) BENCHS+=("$a") ;;
        *) ITERS="$a" ;;
    esac
done
if [ ${#BENCHS[@]} -eq 0 ]; then
    BENCHS=(canada.lua sieve.lua spectralnorm.lua)
fi
TMP="/tmp/clx_diag"
mkdir -p "$TMP"

echo "Rebuilding load() shim with --dynamic --fast..."
if ! ./build/clx benchmarks/run_load_shim.lua -o "$TMP/run_load_shim" --dynamic --fast 2>/dev/null; then
    echo "ERROR: failed to compile load() shim"
    exit 1
fi

cd benchmarks || exit 1
SHIM="$TMP/run_load_shim"

for BENCH in "${BENCHS[@]}"; do
    echo ""
    echo "Running $BENCH through load() $ITERS times..."
    FAILED=""
    for i in $(seq 1 "$ITERS"); do
        if ! "$SHIM" "$BENCH" > /dev/null 2>&1; then
            FAILED="$i"
            break
        fi
        if [ $((i % 20)) -eq 0 ]; then echo "  ...$i ok"; fi
    done

    if [ -z "$FAILED" ]; then
        echo "  No crash in $ITERS runs. All OK."
        continue
    fi

    echo ""
    echo "=== CRASH on iteration $FAILED ==="
    echo "Rerunning once under gdb for a backtrace..."
    gdb -batch -ex run -ex "bt 40" -ex "info registers rsp rbp" --args "$SHIM" "$BENCH" 2>&1 \
        | grep -vE "Debuginfod|debuginfod|For help|Reading symbols|No debugging" | head -60
done
