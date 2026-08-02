#!/bin/bash
# Run the complete test suite through load() and the embedded Lua VM.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

if [ -x "$ROOT_DIR/build/clx" ]; then
    COMPILER="$ROOT_DIR/build/clx"
elif [ -x "$ROOT_DIR/build/bin/clx" ]; then
    COMPILER="$ROOT_DIR/build/bin/clx"
elif command -v clx >/dev/null 2>&1; then
    COMPILER="$(command -v clx)"
else
    echo "Error: clx compiler not found. Run ./build.sh first."
    exit 1
fi

TMP_DIR="${TMPDIR:-/tmp}/clx-load-test.$$"
BIN="$TMP_DIR/run_via_load"
LOG="$TMP_DIR/run_via_load.log"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP_DIR" || exit 1
cd "$ROOT_DIR" || exit 1

echo "Using compiler: $COMPILER"
echo "Compiling tests/run_via_load.lua with --dynamic..."
if ! "$COMPILER" --dynamic tests/run_via_load.lua --output "$BIN"; then
    echo "[FAIL] Could not compile the load-mode test harness."
    exit 1
fi

if [ ! -x "$BIN" ]; then
    echo "[FAIL] Compiler did not produce an executable: $BIN"
    exit 1
fi

echo "Running load-mode test suite..."
"$BIN" >"$LOG" 2>&1
RUN_EXIT=$?
cat "$LOG"

# run_via_load.lua reports aggregate failures in its summary. Check the
# summary as well as the process status because older harness versions did
# not propagate fail_count through their exit status.
if [ "$RUN_EXIT" -eq 0 ] && grep -Eq '^[[:space:]]*fail[[:space:]]*:[[:space:]]*0[[:space:]]*$' "$LOG"; then
    echo "[PASS] load-mode test suite"
    exit 0
fi

echo "[FAIL] load-mode test suite"
if [ "$RUN_EXIT" -ne 0 ]; then
    echo "Harness exit code: $RUN_EXIT"
fi
exit 1
