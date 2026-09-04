#!/bin/bash
# require() with package.path: load real file modules through the embedded VM (--dynamic only).

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

TMP_DIR="${TMPDIR:-/tmp}/clx-require-test.$$"
BIN="$TMP_DIR/require_app"
LOG="$TMP_DIR/require_app.log"
PASS=0
FAIL=0

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP_DIR/app/lib" || exit 1
cd "$ROOT_DIR" || exit 1

echo "Using compiler: $COMPILER"

# A real file module living outside the app directory
cat >"$TMP_DIR/app/lib/greet.lua" <<'EOF'
local M = {}
function M.hello(name)
    return "Hello " .. tostring(name) .. "!"
end
return M
EOF

# A directory-style module (lib/util/init.lua)
mkdir -p "$TMP_DIR/app/lib/util"
cat >"$TMP_DIR/app/lib/util/init.lua" <<'EOF'
return { version = "1.2.3" }
EOF

cat >"$TMP_DIR/app/main.lua" <<'EOF'
local passed = 0
local failed = 0
local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", tostring(actual))
    end
end

-- extend package.path the standard way
package.path = package.path .. ";./lib/?.lua;./lib/?/init.lua"

local greet = require("greet")
assert_eq(greet.hello("package"), "Hello package!", "file module via package.path")

local util = require("util")
assert_eq(util.version, "1.2.3", "directory module via ?/init.lua")

-- require caches: second call returns the same table
local greet2 = require("greet")
assert_eq(greet2, greet, "require caches loaded modules")

-- registered in package.loaded
assert_eq(package.loaded.greet, greet, "module registered in package.loaded")

-- require of a missing module aggregates searcher errors
local ok, err = pcall(require, "missing_module")
if not ok and type(err) == "string" and err:find("missing_module", 1, true) then
    passed = passed + 1
    print("[OK]   ", "missing module reports aggregated error")
else
    failed = failed + 1
    print("[FAIL] ", "missing module reports aggregated error")
end

-- searchpath works inside --dynamic builds too
local f = package.searchpath("greet", "./lib/?.lua")
assert_eq(f, "./lib/greet.lua", "searchpath inside --dynamic build")

print("-----")
print("PASS  :", passed)
print("FAIL  :", failed)
if failed > 0 then
    os.exit(1)
end
EOF

echo "Compiling app with --dynamic..."
if ! "$COMPILER" --dynamic "$TMP_DIR/app/main.lua" --output "$BIN"; then
    echo "[FAIL] Could not compile the require test app."
    exit 1
fi
PASS=$((PASS + 1))

if [ ! -x "$BIN" ]; then
    echo "[FAIL] Compiler did not produce an executable: $BIN"
    exit 1
fi

echo "Running require test app..."
(cd "$TMP_DIR/app" && "$BIN") >"$LOG" 2>&1
RUN_EXIT=$?
cat "$LOG"

if [ "$RUN_EXIT" -eq 0 ] && ! grep -q "\[FAIL\]" "$LOG"; then
    echo "[PASS] require via package.path (--dynamic)"
    exit 0
fi

echo "[FAIL] require via package.path (--dynamic)"
if [ "$RUN_EXIT" -ne 0 ]; then
    echo "App exit code: $RUN_EXIT"
fi
exit 1
