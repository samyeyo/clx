#!/bin/bash
# Run the official Lua 5.5.1 testsuite (this directory) through clx.
# macOS and Linux; Windows uses run.bat, which wraps run.ps1.
# Every test is compiled by clx (harness.lua emits a source file per test); only the
# chunks a test creates at run time with load/loadfile go through the embedded VM.
# One process per test file by default; --all mirrors the upstream single-process run.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

#------------------ the compiler: $CLX wins, then the usual build locations
find_clx() {
    if [ -n "${CLX:-}" ]; then
        printf '%s\n' "$CLX"
        return
    fi
    local candidate
    for candidate in "$ROOT/build/clx" "$ROOT/build/bin/clx" "$ROOT/build/bin/Release/clx" \
                     "$ROOT/bin/clx" "/usr/local/bin/clx" "/usr/bin/clx"; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return
        fi
    done
}

CLX=$(find_clx)
BUILD=${BUILD:-${TMPDIR:-/tmp}}
BUILD=${BUILD%/}/clx-lua55
OUTDIR=$BUILD/out
BINDIR=$OUTDIR/bin
WORK=$BUILD/suite
TIMEOUT=${TIMEOUT:-180}
with_extra=${with_extra:-0}
GEN=$BUILD/harness

#------------------ stdin is an empty pipe, never the terminal and never a file: tests that seek
#------------------ stdin must get a non-seekable stream (stock lua's own suite runs from a tty for
#------------------ the same reason; a redirected empty file would seek successfully)

usage() {
    cat <<EOF
usage: $0 [options] [test.lua ...]

Compiles the vendored Lua 5.5 test suite (this directory) with clx --dynamic and runs it.
With no file arguments every test file is compiled and run, one process each.

options:
  --all          run the suite the upstream way: one process, all.lua order
  --extra        also run bwcoercion.lua and heavy.lua, which upstream keeps out of all.lua
  --list         list the test files instead of running them
  -v|--verbose   print the failing assertion for each failed test
  -h|--help      this message

environment:
  CLX=<path>     clx compiler (default: the repo's build tree)
  TIMEOUT=<sec>  per-test timeout (default: 180)
  SOFT=1         set _soft, skipping the long and memory hungry tests
  PORT=1         set _port, skipping the non-portable tests
  BUILD=<dir>    work + logs directory (default: \${TMPDIR:-/tmp}/clx-lua55)
EOF
}

#------------------ prepare_work: the tests write files next to themselves (libs/A.lua, time.txt,
#------------------ ...), so they run from a scratch copy and the vendored suite stays pristine.
prepare_work() {
    rm -rf "$WORK" || return 1
    mkdir -p "$WORK" || return 1
    cp -R "$HERE"/. "$WORK"/ || return 1
    rm -f "$WORK/run.sh" "$WORK/run.bat" "$WORK/run.ps1" "$WORK/harness.lua"
}

#------------------ build_generator: compile harness.lua, which emits the per-test source files
build_generator() {
    mkdir -p "$OUTDIR" "$BINDIR" || return 1
    rm -f "$GEN"
    "$CLX" "$HERE/harness.lua" --dynamic -o "$GEN" || return 1
    [ -x "$GEN" ]
}

#------------------ build_test: emit "<prelude + test body>" as one chunk and compile it with clx
build_test() {
    local name=$1 src=$OUTDIR/driver_${1%.lua}.lua bin=$BINDIR/${1%.lua}
    ( cd "$WORK" && "$GEN" --emit "$name" "$src" ) || return 1
    rm -f "$bin"
    "$CLX" "$src" --dynamic -o "$bin" >"$OUTDIR/${name%.lua}.compile.log" 2>&1 || return 1
    [ -x "$bin" ]
}

#------------------ build_all: the whole suite in one translation unit, main.lua line removed
build_all() {
    local src=$OUTDIR/driver_all.lua bin=$BINDIR/all
    ( cd "$WORK" && "$GEN" --emit-all "$src" ) || return 1
    rm -f "$bin"
    "$CLX" "$src" --dynamic -o "$bin" >"$OUTDIR/all.compile.log" 2>&1 || return 1
    [ -x "$bin" ]
}

#------------------ list_tests: ask the generator, so every platform lists the same files
list_tests() {
    ( cd "$WORK" && "$GEN" --list $([ "$with_extra" = 1 ] && echo --extra) )
}

#------------------ run_prog: run a command in the scratch copy, killed after $TIMEOUT seconds
#------------------ (macOS ships no timeout(1)). Echoes PASS, FAIL or TIMEOUT.
run_prog() {
    local log=$1 pid waited=0 rc
    shift
    printf '' | ( cd "$WORK" && exec "$@" ) >"$log" 2>&1 &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        waited=$((waited + 1))
        if [ "$waited" -ge "$TIMEOUT" ]; then
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            echo TIMEOUT
            return 0
        fi
    done
    wait "$pid"
    rc=$?
    [ $rc -eq 0 ] && echo PASS || echo FAIL
}

#------------------ last_error: the useful line of a failed log
last_error() {
    grep -E 'assertion failed|cannot open|Error|error:|stack traceback|bad argument' "$1" 2>/dev/null | head -1
}

main() {
    local mode=perfile verbose=0
    local tests=()
    while [ $# -gt 0 ]; do
        case $1 in
            --all) mode=all ;;
            --extra) with_extra=1 ;;
            --list) mode=list ;;
            -v|--verbose) verbose=1 ;;
            -h|--help) usage; return 0 ;;
            -*) echo "unknown option: $1" >&2; usage; return 2 ;;
            *) tests[${#tests[@]}]=$1 ;;
        esac
        shift
    done

    if [ -z "${CLX:-}" ] || [ ! -x "$CLX" ]; then
        echo "clx not found${CLX:+ at $CLX} (build it first, or set CLX=...)" >&2
        return 1
    fi

    if [ "$mode" != list ]; then
        echo "Lua 5.5 suite via: $CLX --dynamic (compiled, one binary per test)"
    fi
    mkdir -p "$OUTDIR" "$BINDIR" || { echo "could not create $OUTDIR" >&2; return 1; }
    prepare_work || { echo "could not stage the suite in $WORK" >&2; return 1; }
    build_generator || { echo "harness build failed" >&2; return 1; }

    if [ "$mode" = list ]; then
        list_tests
        return 0
    fi

    if [ "$mode" = all ]; then
        local log=$OUTDIR/all.log status
        echo "single process, upstream order (main.lua skipped)"
        if ! build_all; then
            echo "COMPILE"
            tail -5 "$OUTDIR/all.compile.log"
            return 1
        fi
        status=$(run_prog "$log" "$BINDIR/all")
        echo "$status"
        [ "$status" = PASS ] && return 0
        echo "  $(last_error "$log")"
        echo "  log: $log"
        return 1
    fi

    if [ ${#tests[@]} -eq 0 ]; then
        local f
        for f in $(list_tests); do
            tests[${#tests[@]}]=$f
        done
    fi

    local pass=0 fail=0 name status log failed=""
    for name in "${tests[@]}"; do
        if [ ! -f "$WORK/$name" ]; then
            echo "missing: $name" >&2
            fail=$((fail + 1))
            continue
        fi
        log=$OUTDIR/${name%.lua}.log
        if ! build_test "$name"; then
            echo "COMPILE $name"
            fail=$((fail + 1))
            failed="$failed $name"
            if [ "$verbose" = 1 ]; then
                echo "        $(tail -1 "$OUTDIR/${name%.lua}.compile.log")"
            fi
            continue
        fi
        status=$(run_prog "$log" "$BINDIR/${name%.lua}")
        printf '%-7s %s\n' "$status" "$name"
        if [ "$status" = PASS ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed="$failed $name"
            if [ "$verbose" = 1 ]; then
                echo "        $(last_error "$log")"
            fi
        fi
    done

    echo "-----"
    echo "pass: $pass  fail: $fail   (logs in $OUTDIR)"
    [ $fail -eq 0 ] || echo "failed:$failed"
    [ $fail -eq 0 ]
}

main "$@"
