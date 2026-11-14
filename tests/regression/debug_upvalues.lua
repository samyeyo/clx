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

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

local function make(v)
    local n = v
    return function() n = n + 1; return n end, function() return n end
end

print("----------------- 1. Reading a shared capture")

local inc, get = make(0)
inc()
inc()
local inc_name, inc_value = debug.getupvalue(inc, 1)
local get_name, get_value = debug.getupvalue(get, 1)
assert_eq(inc_name, "", "compiled closures report an empty upvalue name")
assert_eq(inc_value, 2, "getupvalue reads the captured counter")
assert_eq(get_value, 2, "both closures see the same captured counter")

print("\n----------------- 2. Upvalue identity")

local other, other_get = make(50)
assert_eq(debug.upvalueid(inc, 1) ~= nil, true, "upvalueid is not nil for a capture")
assert_eq(debug.upvalueid(inc, 1) == debug.upvalueid(get, 1), true, "closures sharing a cell share the id")
assert_eq(debug.upvalueid(inc, 1) == debug.upvalueid(other, 1), false, "separate closures get separate ids")
assert_eq(debug.upvalueid(inc, 99), nil, "upvalueid past the last cell is nil")
assert_eq(select("#", debug.upvalueid(inc, 99)), 1, "upvalueid always returns one value")

print("\n----------------- 3. Writing through setupvalue")

assert_eq(debug.setupvalue(inc, 1, 100), "", "setupvalue returns the (empty) name")
assert_eq(get(), 100, "setupvalue writes the cell the other closure reads")
assert_eq(inc(), 101, "the incrementing closure continues from the written value")

print("\n----------------- 4. Range and argument errors")

assert_eq(select("#", debug.getupvalue(inc, 99)), 0, "getupvalue past the last cell returns nothing")
assert_eq(select("#", debug.setupvalue(inc, 99, 1)), 0, "setupvalue past the last cell returns nothing")
assert_eq(pcall(debug.getupvalue, 42, 1), false, "getupvalue rejects a non-function")
assert_eq(pcall(debug.setupvalue, 42, 1, 1), false, "setupvalue rejects a non-function")

print("\n----------------- 5. upvaluejoin is refused")

local joined, join_error = pcall(debug.upvaluejoin, inc, 1, other, 1)
assert_eq(joined, false, "upvaluejoin raises in compiled code")
assert_eq(join_error:find("upvaluejoin is not available in compiled code") ~= nil, true,
    "upvaluejoin names itself in the error")
assert_eq(other_get(), 50, "a refused join leaves the other closure alone")

print("\n----------------- 6. debug library surface")

assert_eq(type(debug), "table", "debug is a global table")
assert_eq(type(debug.getmetatable(1)), "nil", "numbers have no metatable by default")
assert_eq(pcall(debug.getlocal, 1, 1), false, "getlocal raises in compiled code")
assert_eq(pcall(debug.sethook), false, "sethook raises in compiled code")
assert_eq(type(debug.traceback("msg")), "string", "traceback returns a string")

print("\n----------------- 7. Stub errors point at the call site")

local function call_through(f, ...)
    return f(...)
end

local _, info_err = pcall(call_through, debug.getinfo, 1)
assert_eq(info_err:find("debug_upvalues.lua:", 1, true) ~= nil, true,
    "getinfo carries the raising statement's file:line")
assert_eq(info_err:find("debug.getinfo/getlocal", 1, true) ~= nil, true,
    "getinfo keeps its own message after the prefix")
assert_eq(info_err:find("clx:", 1, true), nil, "the location prefix replaces the old clx: marker")

local _, hook_err = pcall(call_through, debug.sethook)
assert_eq(hook_err:find("debug_upvalues.lua:", 1, true) ~= nil, true,
    "sethook carries the raising statement's file:line")

print_summary("debug upvalues")
