local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected: ", expected, " Got: ", actual)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- escaped self-recursive closures")

-- 1. Escaped self-recursive local function: the closure outlives the do-block
-- scope where it was defined. Before the fix, the lambda captured the stack
-- holder (_impl_f, l_f) by reference, so calling t.fn() after the block ended
-- read dead stack storage (SIGSEGV / bad_function_call under ASAN).
local t = {}
do
    local function f(depth)
        if depth <= 0 then return "X" end
        return f(depth - 1) .. "!"
    end
    local g = function() return f(30) end
    t.fn = g
end
assert_eq(t.fn(), "X!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", "escaped self-recursive closure after scope death")

-- 2. Escaped closure that reads its own function value from the body: the body
-- must read through the heap mirror cell, not the dead stack local.
local t2 = {}
do
    local function h(n)
        if n <= 0 then return 0 end
        return n + h(n - 1)
    end
    t2.h = h
end
assert_eq(t2.h(10), 55, "escaped closure body self-read after scope death")

-- 3. Sibling closure defined in the same scope direct-calls the local function;
-- after the scope dies the sibling must still reach the impl via the shared
-- by-value captured cell.
local t3 = {}
do
    local function down(n)
        if n <= 0 then return 0 end
        return down(n - 1) + 1
    end
    t3.run = function() return down(20) end
end
assert_eq(t3.run(), 20, "sibling closure direct-calls escaped local function")

-- 4. Same pattern but the closure escapes by being returned from a function
-- body (defining scope dies when the outer function returns).
local function make_walker()
    local function walk(depth)
        if depth == 0 then return "done" end
        return walk(depth - 1)
    end
    return walk
end
local w = make_walker()
assert_eq(w(50), "done", "self-recursive closure returned from function")

-- 5. Mutual recursion pair escaping via a table: each closure direct-calls the
-- other through the holder cell mechanism.
local t5 = {}
do
    local even, odd
    function even(n) if n == 0 then return true end return odd(n - 1) end
    function odd(n) if n == 0 then return false end return even(n - 1) end
    t5.even = even
    t5.odd = odd
end
assert_eq(t5.even(10), true, "mutual recursion even (escaped)")
assert_eq(t5.odd(10), false, "mutual recursion odd (escaped)")

print_summary("escaped closures")
if failed > 0 then error("escaped closure regression failed") end
