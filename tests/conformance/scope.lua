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

print("----------------- 1. Scope & Shadowing")

local x = 10
do
    local x = 20
    assert_eq(x, 20, "inner block shadow")
end
assert_eq(x, 10, "outer block restore")

local y = 50
local function mutate_y()
    y = 100
end
mutate_y()
assert_eq(y, 100, "upvalue mutation")

local function nested_blocks()
    local val = 1
    do
        local val = 2
        do
            local val = 3
        end
        assert_eq(val, 2, "level 2 shadow restore")
    end
    assert_eq(val, 1, "level 1 shadow restore")
end
nested_blocks()

print("\n----------------- 2. Shared Upvalues")

local function create_shared_scope()
    local val = 10
    local getter = function() return val end
    local setter = function(new_v) val = new_v end
    return getter, setter
end

local get_v, set_v = create_shared_scope()
assert_eq(get_v(), 10, "initial shared upvalue")
set_v(500)
assert_eq(get_v(), 500, "updated shared upvalue via setter")

print("\n----------------- 3. Deep Nested Closures")

local function outer(a)
    return function(b)
        return function(c)
            return a + b + c
        end
    end
end

local add10 = outer(10)
local add10_20 = add10(20)
assert_eq(add10_20(30), 60, "triple nested capture (10+20+30)")

print("\n----------------- 4. Tail Call Optimization (TCO)")

-- This will cause a stack overflow in C++ if TCO is not working
local function tail_recursion(n, acc)
    if n == 0 then return acc end
    return tail_recursion(n - 1, acc + 1)
end

-- 100,000 calls is a standard depth to verify TCO
local tco_res = tail_recursion(100000, 0)
assert_eq(tco_res, 100000, "TCO recursion depth (100k)")

print("\n----------------- 5. Variable Lifecycle in Loops")

local funcs = {}
for i = 1, 3 do
    local iteration_val = i * 10
    funcs[i] = function() return iteration_val end
end

assert_eq(funcs[1](), 10, "loop variable capture iteration 1")
assert_eq(funcs[2](), 20, "loop variable capture iteration 2")
assert_eq(funcs[3](), 30, "loop variable capture iteration 3")

print("\n----------------- 6. Goto and Labels")

local x_loop = 0

::start_loop::
x_loop = x_loop + 1

if x_loop < 3 then
    goto start_loop
end

assert_eq(x_loop, 3, "Goto successfully creates a loop")

-- Testing the Lexical Label Resolution Pass (Duplicate Labels in Different Scopes)
local y_dup = 0
do
    ::duplicate_label::
    y_dup = y_dup + 1
    if y_dup < 2 then goto duplicate_label end
end

local z_dup = 0
do
    ::duplicate_label::
    z_dup = z_dup + 1
    if z_dup < 3 then goto duplicate_label end
end

assert_eq(y_dup, 2, "First duplicate label resolved correctly")
assert_eq(z_dup, 3, "Second duplicate label resolved correctly")

print_summary("SCOPE, TCO & CLOSURES")