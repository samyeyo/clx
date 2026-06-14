#!/bin/bash
set -eu

cd "$(dirname "$0")/.." || exit 1

RUNS=10
WARMUP=3
TEST_DIR="benchmarks"
CLX_CMD="./build/clx"
CPPFLAGS="${CPPFLAGS:---fast}"
TMPDIR="${TMPDIR:-/tmp}/clx_bench"
mkdir -p "$TMPDIR"

# Check required commands
for cmd in lua luajit awk; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "Error: '$cmd' is not installed or not in PATH."
        exit 1
    fi
done

if [ ! -f "$CLX_CMD" ]; then
    echo "Error: CLX not built. Run ./build.sh first."
    exit 1
fi

ulimit -s unlimited 2>/dev/null || true

# CPU pinning + performance governor
CPU_PIN=""
GOV_SAVED=""
if command -v taskset &>/dev/null; then
    CPU_PIN="taskset -c 0"
    if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
        GOV_SAVED=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
        echo "performance" | tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor &>/dev/null || true
    fi
fi

cleanup() {
    if [ -n "$GOV_SAVED" ]; then
        echo "$GOV_SAVED" | tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor &>/dev/null || true
    fi
}
trap cleanup EXIT

# Use EPOCHREALTIME (bash 5.0+ builtin) for nanosecond precision without subprocess overhead.
# Fallback: date +%s%N (subprocess overhead, but functional).
if [ -n "${EPOCHREALTIME:-}" ]; then
    time_single() {
        local cmd="$1" t
        t=$EPOCHREALTIME
        local start_ns=$((${t%.*} * 1000000000 + 10#${t#*.} * 1000))
        eval "$cmd" > /dev/null 2>&1 || true
        t=$EPOCHREALTIME
        local end_ns=$((${t%.*} * 1000000000 + 10#${t#*.} * 1000))
        echo $((end_ns - start_ns))
    }
else
    time_single() {
        local cmd="$1"
        local start end
        start=$(date +%s%N)
        eval "$cmd" > /dev/null 2>&1 || true
        end=$(date +%s%N)
        echo $((end - start))
    }
fi

time_engine() {
    local cmd="$1"
    local i t total=0
    local runner="${CPU_PIN:+$CPU_PIN }"
    local full_cmd="${runner}${cmd}"

    for i in $(seq 1 $WARMUP); do
        eval "$full_cmd" > /dev/null 2>&1
    done

    for i in $(seq 1 $RUNS); do
        t=$(time_single "$full_cmd")
        total=$((total + t))
    done

    awk "BEGIN {printf \"%.3f\", $total / 1000000000 / $RUNS}"
}

echo "Starting benchmarks ($RUNS runs per script)..."
echo "================================================================================"
printf "%-22s | %-15s | %-18s | %-18s\n" "Script" "Lua 5.5" "LuaJIT" "clx"
echo "================================================================================"

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

    # Remove stale binary before compiling
    rm -f "$clx_exe"

    # Multi-module benchmarks
    extra=""
    case "$basename" in
        canada) extra="$TEST_DIR/dkjson.lua" ;;
    esac

    if ! $CLX_CMD "$file" $extra --output "$clx_exe" $CPPFLAGS 2>/dev/null; then
        printf "%-22s | %-15s | %-18s | %-18s\n" "$basename.lua" "CLX COMPILE FAIL" "-" "-"
        continue
    fi

    if [ ! -x "$clx_exe" ]; then
        printf "%-22s | %-15s | %-18s | %-18s\n" "$basename.lua" "CLX COMPILE FAIL" "-" "-"
        continue
    fi

    avg_lua=$(time_engine "sh -c 'cd \"$TEST_DIR\" && exec lua \"$(basename "$file")\"'")
    avg_luajit=$(time_engine "sh -c 'cd \"$TEST_DIR\" && exec luajit \"$(basename "$luajit_file")\"'")
    avg_clx=$(time_engine "sh -c 'cd \"$TEST_DIR\" && exec \"$clx_exe\"'")

    # Speedups
    sp_luajit=$(awk "BEGIN {printf \"%.2fx\", $avg_lua / $avg_luajit}")
    sp_clx=$(awk "BEGIN {printf \"%.2fx\", $avg_lua / $avg_clx}")

    printf "%-22s | %-5ss (1.00x) | %-5ss (%-6s)   | %-5ss (%-6s)\n" \
        "$basename.lua" \
        "$avg_lua" \
        "$avg_luajit" "$sp_luajit" \
        "$avg_clx" "$sp_clx"

    rm -f "$clx_exe"
done

if [ $FOUND_FILES -eq 0 ]; then
    echo "No .lua scripts found in $TEST_DIR/."
fi

echo "============================================================================="
echo "Benchmarking complete."
