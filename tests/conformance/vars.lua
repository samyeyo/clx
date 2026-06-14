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

print("----------------- 1. Standard Locals")

local a = 10
local c, d = 30, 40
assert_eq(a, 10, "basic local assignment")
assert_eq(c, 30, "multiple local assignment")

print("\n----------------- 2. Lua 5.5 Explicit Globals")

global explicit_b = 20
global explicit_e, explicit_f = 50, 60

assert_eq(explicit_b, 20, "explicit single global")
assert_eq(explicit_e, 50, "explicit multiple global")

print("\n----------------- 3. Lua 5.5 Global Functions")

global function my_global_func()
    return "hello from global func"
end
assert_eq(my_global_func(), "hello from global func", "explicit global function")

print("\n----------------- 4. Lua 5.5 Const Globals")

global <const> STRICT_CONST = 3.14
assert_eq(STRICT_CONST, 3.14, "explicit const global")

print("\n----------------- 5. Lua 5.5 Block Scoping & Wildcards")

do
    global *
    
    legacy_implicit_var = 777
    assert_eq(legacy_implicit_var, 777, "implicit global via wildcard 'global *'")
end

do
    global <const> *
    
    local read_test = explicit_b
    assert_eq(read_test, 20, "reading global under 'global <const> *'")
end

print("\n----------------- 6. lua 5.5 Attributes")

local c_val <const> = 42
assert_eq(c_val, 42, "<const> local variable")

local close_order = 0
do
    local res1 = { name = "first" }
    setmetatable(res1, { __close = function() close_order = close_order + 1 end })
    local guard1 <close> = res1
end
assert_eq(close_order, 1, "<close> local resource destructed")

print("\n----------------- 7. Multi-Assignment Swapping")

local swap_a, swap_b = 100, 200
swap_a, swap_b = swap_b, swap_a
assert_eq(swap_a, 200, "swap_a received swap_b's value")
assert_eq(swap_b, 100, "swap_b received swap_a's original value")

local x, y, z = 1, 2, 3
x, y, z = y, z, x
assert_eq(x, 2, "3-way swap: x gets y")
assert_eq(y, 3, "3-way swap: y gets z")
assert_eq(z, 1, "3-way swap: z gets x")

print("\n----------------- 8. Variable Shadowing (Local Redeclaration)")

local shadow_var = 100

do
    local shadow_var = 200
    assert_eq(shadow_var, 200, "inner shadowed variable")
end

assert_eq(shadow_var, 100, "outer variable restored after block")

local shadow_var = 999
assert_eq(shadow_var, 999, "local redeclaration in the same scope")

print("\n----------------- 9. Global Variable Redeclaration")

global explicit_redecl = 333
assert_eq(explicit_redecl, 333, "first explicit global declaration")

explicit_redecl = "Hello"
assert_eq(explicit_redecl, "Hello", "explicit global redeclaration in same scope")

print_summary("VARIABLES & LUA 5.5 SCOPING")