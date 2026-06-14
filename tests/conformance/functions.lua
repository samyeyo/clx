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

print("----------------- 1. Standard Functions & Closures")

local function mult_ret()
    return 10, 20
end

local r1, r2 = mult_ret()
assert_eq(r1, 10, "multiple returns 1")
assert_eq(r2, 20, "multiple returns 2")

local function make_counter()
    local count = 0
    return function()
        count = count + 1
        return count
    end
end

local c1 = make_counter()
assert_eq(c1(), 1, "closure upvalue step 1")
assert_eq(c1(), 2, "closure upvalue step 2")

local c2 = make_counter()
assert_eq(c2(), 1, "closure independent instance")
assert_eq(c1(), 3, "closure upvalue step 3")

print("\n----------------- 2. Lua 5.5 Named Varargs")

-- Lua 5.5 syntax: ... [name] automatically packs arguments into a table
-- The table also includes the '.n' field for the argument count.
local function test_named_varargs(a, b, ... extra)
    assert_eq(a, 1, "first fixed param")
    assert_eq(b, 2, "second fixed param")
    
    assert_eq(type(extra), "table", "varargs packed into table 'extra'")
    assert_eq(extra.n, 3, "vararg count .n is correct")
    assert_eq(extra[1], "x", "vararg index 1")
    assert_eq(extra[2], "y", "vararg index 2")
    assert_eq(extra[3], "z", "vararg index 3")
end

test_named_varargs(1, 2, "x", "y", "z")

-- Testing empty named varargs
local function test_empty_varargs(... args)
    assert_eq(args.n, 0, "empty vararg count is 0")
end

test_empty_varargs()

print("\n----------------- 3. Recursive Local Functions")

local function factorial(n)
    if n <= 1 then return 1 end
    return n * factorial(n - 1)
end

assert_eq(factorial(5), 120, "simple recursion (factorial 5)")
assert_eq(factorial(1), 1, "recursion base case")
assert_eq(factorial(0), 1, "recursion zero case")

local function fibonacci(n)
    if n <= 1 then return n end
    return fibonacci(n - 1) + fibonacci(n - 2)
end

assert_eq(fibonacci(10), 55, "double recursion (fib 10)")
assert_eq(fibonacci(0), 0, "fib base case 0")
assert_eq(fibonacci(1), 1, "fib base case 1")

print("\n----------------- 4. Advanced MultiValue Unpacking")

local function bar(a, b, c)
    return 4, 8, 15, 16, 23, 42
end

local function pack(...)
    local args = {...}
    args.n = select("#", ...)
    return args
end

-- Rule 1: A function call expands to all its return values if it is the LAST item in an expression list.
local x, y = bar('zaphod')
assert_eq(x, 4, "multivalue assignment first var")
assert_eq(y, 8, "multivalue assignment second var")

local t1 = pack(1, bar('zaphod'))
assert_eq(t1.n, 7, "bar expands fully at the end of pack")
assert_eq(t1[1], 1, "pack index 1")
assert_eq(t1[2], 4, "pack index 2")
assert_eq(t1[7], 42, "pack index 7")

-- Rule 2: A function call is truncated to exactly ONE return value if it is NOT the last item in an expression list.
local t2 = pack(bar('zaphod'), 1)
assert_eq(t2.n, 2, "bar truncated to 1 value before another argument")
assert_eq(t2[1], 4, "truncated bar returns only first value")
assert_eq(t2[2], 1, "the second argument follows immediately")

-- Rule 3: Function calls wrapped in parentheses are ALWAYS truncated to exactly one value.
local t3 = pack((bar('zaphod')))
assert_eq(t3.n, 1, "parentheses truncate to 1 value")
assert_eq(t3[1], 4, "parentheses yield only first value")

print("\n----------------- 5. Function Redeclaration")

local function local_redecl() return 100 end
assert_eq(local_redecl(), 100, "initial local function")

local function local_redecl() return 200 end
assert_eq(local_redecl(), 200, "redeclared local function in same scope")

-- Global function redeclaration
local function make_global()
    global function global_redecl() return 300 end
end

make_global()
assert_eq(global_redecl(), 300, "initial global function")

global function global_redecl() return 400 end
assert_eq(global_redecl(), 400, "redeclared global function")

local f = function(x) return x + 1 end
f = function(xx) return xx * 10 end
assert_eq(f(5), 50, "anonymous function reassignment")

print_summary("FUNCTIONS")