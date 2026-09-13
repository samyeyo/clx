local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", actual)
    end
end

local function assert_true(val, name)
    if val then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- const-captured table survives GC after scope death")

-- Regression: parse_statement_fns in tlcli.lua was a table captured by value
-- into escaping parser closures (constant_upvalues). Its only GC root was the
-- defining block's shadow slot; once the block ended, the table was swept and
-- its memory reused (token records), so live closures read token data as
-- function values. The fix roots const-captured locals via _csnap cells
-- passed to create_closure, which the GC marker live-dereferences.

local dispatch = {}

do
    -- Table captured by value into the closures below (never reassigned).
    local parse_statement_fns = { kind = "dispatch" }
    for i = 1, 8 do
        parse_statement_fns["k" .. i] = function(x)
            return "fn" .. i .. "(" .. tostring(x) .. ")"
        end
    end

    do
        -- Escaped closure capturing the table by value; forces the same
        -- constant_upvalues path as tlcli's parser.
        local function make_caller(name)
            return function(x)
                local fn = parse_statement_fns[name]
                if type(fn) ~= "function" then
                    return "BAD:" .. type(fn) .. ":" .. tostring(fn)
                end
                return fn(x)
            end
        end
        for i = 1, 8 do
            dispatch["call" .. i] = make_caller("k" .. i)
        end
        dispatch.table_fn = function()
            return parse_statement_fns.kind
        end
    end
end

collectgarbage("collect")
collectgarbage("collect")

for i = 1, 8 do
    assert_eq(dispatch["call" .. i](i), "fn" .. i .. "(" .. i .. ")", "dispatch call " .. i .. " after GC")
end
assert_eq(dispatch.table_fn(), "dispatch", "captured table field after GC")

print("----------------- forward-declared function table under GC pressure")

-- Pattern from tlcli: forward-declared locals assigned inside do-blocks,
-- dispatching through the table after the defining scopes are dead.
local kw_handlers = {}
do
    local handlers = { n = 0 }
    local function register(name, fn)
        handlers[name] = fn
    end
    local function get(name)
        local fn = handlers[name]
        if type(fn) ~= "function" then
            return "BAD:" .. type(fn)
        end
        return fn
    end
    for i = 1, 10 do
        register("h" .. i, function(v)
            return v * i
        end)
    end
    kw_handlers.get = get
    kw_handlers.count = function()
        handlers.n = handlers.n + 1
        return handlers.n
    end
end

collectgarbage("collect")

assert_eq(kw_handlers.get("h7")(3), 21, "handler from captured table after GC")
assert_eq(kw_handlers.get("h10")(4), 40, "second handler after GC")
assert_eq(kw_handlers.count(), 1, "mutable state in captured table alive")
assert_eq(kw_handlers.count(), 2, "mutable state increments")

print("----------------- GC pressure with repeated churn")

-- Repeated cycles: build escaped closures over const-captured tables, force
-- collection, verify. Stresses free-list reuse of LCFunctions and tables.
local results = {}
for round = 1, 5 do
    local keep
    do
        local shared = { tag = "r" .. round }
        local fns = {}
        for i = 1, 20 do
            fns["f" .. i] = function()
                return shared.tag .. ":" .. i
            end
        end
        keep = function(i)
            local fn = fns["f" .. i]
            if type(fn) ~= "function" then
                return "BAD:" .. type(fn)
            end
            return fn()
        end
    end
    collectgarbage("collect")
    collectgarbage("step")
    collectgarbage("collect")
    results[round] = keep(7) .. "|" .. keep(20)
end

assert_eq(results[1], "r1:7|r1:20", "round 1 result")
assert_eq(results[2], "r2:7|r2:20", "round 2 result")
assert_eq(results[3], "r3:7|r3:20", "round 3 result")
assert_eq(results[4], "r4:7|r4:20", "round 4 result")
assert_eq(results[5], "r5:7|r5:20", "round 5 result")

print_summary("GC_CELL_ROOTING")
if failed > 0 then
    error("gc_cell_rooting: " .. failed .. " failures")
end
