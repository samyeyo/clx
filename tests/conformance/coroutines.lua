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
    print("-----\nSUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed, "\n-----")
end

print("----------------- 1. Basic Creation and Status")

local co1 = coroutine.create(function() return 42 end)
assert_eq(coroutine.status(co1), "suspended", "status is suspended after creation")

local success, res = coroutine.resume(co1)
assert_eq(success, true, "resume successful")
assert_eq(res, 42, "correct return value")
assert_eq(coroutine.status(co1), "dead", "status is dead after return")

print("\n----------------- 2. Yield and Resume Values")

local co2 = coroutine.create(function(a, b)
    local c = coroutine.yield(a + b)
    return c * 2
end)

local s1, y1 = coroutine.resume(co2, 10, 20)
assert_eq(s1, true, "first resume successful")
assert_eq(y1, 30, "yielded sum")
assert_eq(coroutine.status(co2), "suspended", "status is suspended after yield")

local s2, y2 = coroutine.resume(co2, 50)
assert_eq(s2, true, "second resume successful")
assert_eq(y2, 100, "returned multiplied value")

local s3, y3 = coroutine.resume(co2)
assert_eq(s3, false, "cannot resume dead coroutine")

print("\n----------------- 3. Deeply Nested Yielding (Stackful Test)")

local function deep_func(depth)
    if depth == 0 then
        coroutine.yield("bottom")
        return "done"
    end
    return deep_func(depth - 1)
end

local co3 = coroutine.create(function()
    return deep_func(5)
end)

local ds1, dval1 = coroutine.resume(co3)
assert_eq(ds1, true, "deep resume 1 successful")
assert_eq(dval1, "bottom", "yielded from depth 5")

local ds2, dval2 = coroutine.resume(co3)
assert_eq(ds2, true, "deep resume 2 successful")
assert_eq(dval2, "done", "returned from depth 5")

print("\n----------------- 4. Coroutine as Iterator (manual iteration)")

local function number_producer()
    coroutine.yield(1)
    coroutine.yield(2)
    coroutine.yield(3)
end

local co = coroutine.create(number_producer)
local result1 = {coroutine.resume(co)}
assert_eq(result1[1], true, "first resume succeeds")
assert_eq(result1[2], 1, "first yielded value")

local result2 = {coroutine.resume(co)}
assert_eq(result2[1], true, "second resume succeeds")
assert_eq(result2[2], 2, "second yielded value")

local result3 = {coroutine.resume(co)}
assert_eq(result3[1], true, "third resume succeeds")
assert_eq(result3[2], 3, "third yielded value")

local result4 = {coroutine.resume(co)}
local status = coroutine.status(co)
assert_eq(status, "dead", "coroutine is dead after exhaustion")

print("\n----------------- 5. coroutine.wrap() with for..in Loop")

local function number_producer2(max)
    for i = 1, max do
        coroutine.yield(i)
    end
end

local wrap2 = coroutine.wrap(number_producer2)
local sum = 0
local val = wrap2(10)
while val ~= nil do
    sum = sum + val
    val = wrap2()
end
assert_eq(sum, 55, "wrap with loop sums 1-10")

local wrap3 = coroutine.wrap(function()
    coroutine.yield("a")
    coroutine.yield("b")
    coroutine.yield("c")
end)
local count = 0
local values = {}
local v = wrap3()
while v ~= nil do
    count = count + 1
    values[count] = v
    v = wrap3()
end
assert_eq(count, 3, "wrap with loop yields 3 values")
assert_eq(values[1], "a", "first yielded value")
assert_eq(values[2], "b", "second yielded value")
assert_eq(values[3], "c", "third yielded value")

local function fibonacci()
    local a, b = 0, 1
    for i = 1, 10 do
        local c = a + b
        a, b = b, c
        coroutine.yield(a)
    end
end

local fib_wrap = coroutine.wrap(fibonacci)
local fib_count = 0
local fib_sum = 0
local fv = fib_wrap()
while fv ~= nil do
    fib_count = fib_count + 1
    fib_sum = fib_sum + fv
    fv = fib_wrap()
end
assert_eq(fib_count, 10, "fibonacci wrap iterated 10 times")
assert_eq(fib_sum, 143, "sum of first 10 fibonacci numbers")

print_summary("COROUTINES")