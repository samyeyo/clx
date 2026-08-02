-- ─────────────────────────────────────────────
-- │ clx — Lua to C++ Native Compiler           │
-- │  run_via_load.lua · dynamic-load smoke     │
-- ─────────────────────────────────────────────
--
-- Exercises clx's embedded Lua 5.5 VM (`load`/`loadfile`/
-- `dofile`, registered by clx_register_load_builtins when
-- the user binary is compiled with `--dynamic`) on the ACTUAL clx
-- test suite.
--
-- Two modes:
--
--   COORDINATOR (default, no arg)
--       Iterates the FILES list. For each test, spawns a FRESH child
--       process (via os.execute) that handles exactly one file. This
--       is critical: clx's bridge (NativeBridge + DynamicVM) is
--       unstable across long sequences of load() + pcall() in a
--       single process — we observed segfaults after ~18 consecutive
--       tests. One file per process side-steps that accumulation.
--
--   WORKER (single arg: relative path to a .lua test file)
--       Loads that one file via load(content, path, "t"), executes
--       via pcall. Reports "OK: <path>" or "FAIL: <path> -- <err>"
--       on a single line. Exits 0 on OK, non-zero on FAIL.
--
-- Build:
--   ./build/clx --dynamic tests/run_via_load.lua -o run_via_load
--
-- Run from the project root:
--   ./run_via_load
--
-- Output answers: "do all the tests in ./tests/ pass using load()?".
-- ─────────────────────────────────────────────

-- Make realworld tests resolve their require("lunajson.encoder")-style
-- imports. Stock Lua 5.5's package.path is the host Lua's install
-- paths; we extend the search so require() can find sibling libraries.
package.path = (package.path or "")
    .. ";./?.lua"
    .. ";./tests/?.lua"
    .. ";./tests/conformance/?.lua"
    .. ";./tests/regression/?.lua"
    .. ";./tests/edge_cases/?.lua"
    .. ";./tests/stress/?.lua"
    .. ";./tests/realworld/?.lua"
    .. ";./tests/realworld/?/?.lua"

-- Only standalone test programs are listed. Library files inside
-- realworld/* (e.g. lunajson/encoder.lua) are reachable via require()
-- from the matching test.lua, not standalone programs. conformance/
-- _diag.lua and mymod.lua are helpers, not tests. test_native_mod.lua
-- loads C++-side symbols that aren't exposed in --dynamic mode.
local FILES = {
    -- conformance
    "tests/conformance/arithmetic.lua",
    "tests/conformance/base_api.lua",
    "tests/conformance/conditions.lua",
    "tests/conformance/control_flow.lua",
    "tests/conformance/coroutines.lua",
    "tests/conformance/error.lua",
    "tests/conformance/functions.lua",
    "tests/conformance/gc.lua",
    "tests/conformance/io.lua",
    "tests/conformance/load.lua",
    "tests/conformance/logic.lua",
    "tests/conformance/loops.lua",
    "tests/conformance/metatable.lua",
    "tests/conformance/operators.lua",
    "tests/conformance/os.lua",
    "tests/conformance/package.lua",
    "tests/conformance/scope.lua",
    "tests/conformance/string.lua",
    "tests/conformance/strings.lua",
    "tests/conformance/table.lua",
    "tests/conformance/utf8.lua",
    "tests/conformance/vars.lua",

    -- regression
    "tests/regression/cacheslot.lua",
    "tests/regression/edgecases.lua",
    "tests/regression/for_in_native.lua",
    -- goto_hoist.lua uses goto to jump into the scope of a local variable.
    -- Lua 5.5's parser rejects this (<goto> jumps into the scope of 'z').
    -- clx's AOT compiler is more lenient and allows it. Since this file is
    -- INTENTIONALLY invalid Lua 5.5, it cannot be loaded via load().
    -- "tests/regression/goto_hoist.lua",
    "tests/regression/non_numeric_keys.lua",
    "tests/regression/pure_numeric_array.lua",

    -- edge_cases
    "tests/edge_cases/deep_binops.lua",
    "tests/edge_cases/massive_table.lua",
    "tests/edge_cases/scope_hell.lua",
    "tests/edge_cases/stringbuilder.lua",
    "tests/edge_cases/type_inference.lua",

    -- stress
    "tests/stress/stress.lua",

    -- realworld (test.lua only — requires its sibling library)
    "tests/realworld/30log/test.lua",
    "tests/realworld/ansicolors/test.lua",
    "tests/realworld/argparse/test.lua",
    "tests/realworld/classic/test.lua",
    "tests/realworld/dkjson/test.lua",
    "tests/realworld/fun/test.lua",
    "tests/realworld/inspect/test.lua",
    "tests/realworld/lume/test.lua",
    "tests/realworld/lunajson/test.lua",
    "tests/realworld/middleclass/test.lua",
    "tests/realworld/serpent/test.lua",
}

-- WORKER MODE: process exactly one file referenced by arg[1].
local function worker_run(path)
    local f, ferr = io.open(path, "rb")
    if not f then
        io.write(string.format("FAIL %s -- io.open: %s\n",
            path, tostring(ferr)))
        io.flush()
        return 1
    end
    local content = f:read("*a")
    f:close()
    if not content then
        io.write(string.format("FAIL %s -- io.read returned nil\n", path))
        io.flush()
        return 1
    end
    -- Prepend `global *` at chunk level when the file uses the `global`
    -- keyword. This allows all globals (print, assert, etc.) without
    -- requiring explicit `global` declarations, matching Lua 5.5's
    -- explicit-globals opt-out. Files without `global` keyword don't
    -- need the preamble.
    --
    -- Note: we DO NOT strip `global` from explicit declarations because
    -- `global function name()` inside a function body creates a GLOBAL
    -- function, while plain `function name()` creates a LOCAL. The file
    -- itself should use `function` instead of `global function` for any
    -- declaration that follows a `global *` line — at that point the
    -- `global` keyword is redundant and plain `function name()` is both
    -- cleaner and avoids redeclaration errors in Lua 5.5's parser.
    local load_content = content
    if content:find("\nglobal[ \t]") then
        load_content = "global *\n" .. content
    end
    local chunk, lerr = load(load_content, path, "t")
    if not chunk then
        io.write(string.format("FAIL %s -- load: %s\n", path, tostring(lerr)))
        io.flush()
        return 1
    end
    local ok, perr = pcall(chunk)
    if not ok then
        io.write(string.format("FAIL %s -- pcall: %s\n",
            path, tostring(perr)))
        io.flush()
        return 1
    end
    io.write(string.format("OK   %s\n", path))
    io.flush()
    return 0
end

-- COORDINATOR MODE: iterate FILES and spawn a fresh child per file.
-- Each child is `./run_via_load path`, which exits 0 on OK and 1
-- on FAIL. We aggregate via the exit code.
local function coordinator_run()
    -- arg[0] in clx's LState is the path used to invoke this binary.
    -- That lets us re-exec ourselves for each file, regardless of $PATH.
    local self = arg and arg[0] or "./run_via_load"

    local ok_count, fail_count = 0, 0
    local failures = {}

    io.write(string.format("[run_via_load] profiling %d test files (one process per file)\n", #FILES))
    io.flush()

    for i, path in ipairs(FILES) do
        local cmd = string.format('%s %q', self, path)
        local rc = os.execute(cmd)
        -- clx's os.execute returns 0 on success, non-zero on failure,
        -- matching stock Lua's convention.
        if rc == 0 then
            ok_count = ok_count + 1
            io.write(string.format("[%3d/%3d] %s\n", i, #FILES, "OK   " .. path))
        else
            fail_count = fail_count + 1
            failures[#failures + 1] = path
            io.write(string.format("[%3d/%3d] FAIL %s\n", i, #FILES, path))
        end
        io.flush()
    end

    io.write("\n[run_via_load] === summary ===\n")
    io.write(string.format("  total : %d\n", #FILES))
    io.write(string.format("  ok    : %d\n", ok_count))
    io.write(string.format("  fail  : %d\n", fail_count))

    if fail_count > 0 then
        io.write("\n[run_via_load] === failures ===\n")
        for i, p in ipairs(failures) do
            -- Re-run the failing file once more, this time without --quiet
            -- semantics, so we capture the actual error string in stdout.
            local cmd = string.format('%s %q', self, p)
            local f = io.popen and nil  -- io.popen is nil in clx; skip
            io.write(string.format("  - %s\n", p))
        end
        io.write(string.format(
            "\n[run_via_load] to see details, run: %s <file>\n", self))
    end

    io.write("\n[run_via_load] done\n")
    io.flush()
end

-- Dispatch by presence of arg[1].
if arg and arg[1] then
    os.exit(worker_run(arg[1]))
else
    coordinator_run()
end
