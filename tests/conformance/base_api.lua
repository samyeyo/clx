local passed = 0
local failed = 0

local function assert_num_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name)
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

local function assert_false(val, name)
    if val then
        failed = failed + 1
        print("[FAIL] ", name)
    else
        passed = passed + 1
        print("[OK]   ", name)
    end
end

local function assert_str_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", actual)
    end
end

local function assert_nil(val, name)
    if val == nil then
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

print("----------------- _VERSION")
assert_str_eq(_VERSION, "lua 5.5", "_VERSION global")

print("\n----------------- rawequal")
assert_true(rawequal(10, 10), "rawequal same ints")
assert_false(rawequal(10, 20), "rawequal diff ints")
assert_true(rawequal(3.14, 3.14), "rawequal same floats")
assert_false(rawequal(3.14, 2.71), "rawequal diff floats")
assert_true(rawequal(10, 10.0), "rawequal int vs float same")
assert_true(rawequal("abc", "abc"), "rawequal same strings")
assert_false(rawequal("abc", "xyz"), "rawequal diff strings")
assert_true(rawequal(true, true), "rawequal same bools")
assert_false(rawequal(true, false), "rawequal diff bools")
assert_true(rawequal(nil, nil), "rawequal nil nil")
assert_false(rawequal(nil, false), "rawequal nil vs false")
assert_false(rawequal(42, "42"), "rawequal int vs string")

local t1 = { x = 1 }
local t2 = { x = 1 }
assert_true(rawequal(t1, t1), "rawequal same table ref")
assert_false(rawequal(t1, t2), "rawequal diff table ref")

local mt = { __eq = function(a, b) return true end }
local ta = {}
local tb = {}
setmetatable(ta, mt)
setmetatable(tb, mt)
assert_true(ta == tb, "__eq makes tables equal")
assert_false(rawequal(ta, tb), "rawequal bypasses __eq")

print("\n----------------- rawget")
local rg_t = { a = 1, b = 2, c = 3 }
assert_num_eq(rawget(rg_t, "a"), 1, "rawget existing key")
assert_nil(rawget(rg_t, "z"), "rawget missing key")

local rg_idx = { __index = function(t, k) return "default" end }
local rg_empty = {}
setmetatable(rg_empty, rg_idx)
assert_str_eq(rg_empty.x, "default", "__index provides default")
assert_nil(rawget(rg_empty, "x"), "rawget bypasses __index")

print("\n----------------- rawset")
local rs_t = {}
rawset(rs_t, "key1", 100)
assert_num_eq(rs_t.key1, 100, "rawset sets value")

rawset(rs_t, "key1", 200)
assert_num_eq(rs_t.key1, 200, "rawset overwrites value")

local rs_block = {}
local rs_mt = { __newindex = function(t, k, v) end }
setmetatable(rs_block, rs_mt)
rs_block.hidden = "blocked"
assert_nil(rs_block.hidden, "__newindex blocks set")
rawset(rs_block, "hidden", "bypass")
assert_str_eq(rawget(rs_block, "hidden"), "bypass", "rawset bypasses __newindex")

print("\n----------------- rawlen")
assert_num_eq(rawlen("hello"), 5, "rawlen string")
assert_num_eq(rawlen(""), 0, "rawlen empty string")
assert_num_eq(rawlen({10, 20, 30}), 3, "rawlen table array")
assert_num_eq(rawlen({}), 0, "rawlen empty table")

local rl_mt = { __len = function(t) return 999 end }
local rl_t = {10, 20}
setmetatable(rl_t, rl_mt)
assert_num_eq(#rl_t, 999, "__len provides length")
assert_num_eq(rawlen(rl_t), 2, "rawlen bypasses __len")

print("\n----------------- warn")
warn("test warning message")
warn("@off")
warn("this should not appear")
warn("@on")
warn("warnings re-enabled")

passed = passed + 1
print("[OK]   ", "warn does not crash")

print_summary("BASE_API")
