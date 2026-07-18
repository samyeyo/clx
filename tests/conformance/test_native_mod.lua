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

print("----------------- Native Module via --modules")

local m = require("native_mod")

assert_eq(type(m), "table", "module is a table")
assert_eq(type(m.add), "function", "module.add is a function")
assert_eq(type(m.greet), "function", "module.greet is a function")
assert_eq(type(m.get_table), "function", "module.get_table is a function")

-- Test add
local sum = m.add(10, 20, 30)
assert_eq(sum, 60, "add(10, 20, 30) = 60")

-- Test greet
local msg = m.greet("world")
assert_eq(msg, "hello world", "greet('world')")

-- Test get_table
local t2 = m.get_table()
assert_eq(type(t2), "table", "get_table() returns a table")
assert_eq(t2.x, 10, "t2.x = 10")
assert_eq(t2.y, 20, "t2.y = 20")
assert_eq(t2.label, "point", 't2.label = "point"')

-- Test test_api
local r = m.test_api()
assert_eq(r, 3, "test_api() = 3 (via check_field_number + raw_set/raw_get)")

-- Test require(mod, env) — C++ side: luaopen receives env
local myenv = { greeting = "hello" }
package.loaded["native_mod"] = nil
local m2 = require("native_mod", myenv)
assert_eq(m2.get_env(), myenv, "luaopen received env from require")

-- Test require without env (default nil) — _G is the real global table
package.loaded["native_mod"] = nil
local m3 = require("native_mod")
assert_eq(m3.get_env(), _G, "require without env gives module the real _G")

print_summary("NATIVE_MODULE")
os.exit(failed > 0 and 1 or 0)
