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

local function print_summary()
    print("-----")
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
    os.exit(failed > 0 and 1 or 0)
end

print("----------------- coroutine recycle regression")

local function churn(n)
    for i = 1, n do
        local co = coroutine.create(function() return i end)
        coroutine.resume(co)
    end
end

-- 1) a body abandoned while suspended keeps its continuation to itself
local ran = 0
for i = 1, 100 do
    local co = coroutine.create(function()
        coroutine.yield(1)
        ran = ran + 1
    end)
    coroutine.resume(co)
end
collectgarbage("collect")
collectgarbage("collect")
churn(100)
collectgarbage("collect")
assert_eq(ran, 0, "an abandoned suspended body never resumes")

-- 2) a generator abandoned mid-loop stops at its yield instead of running on
local iterated = 0
for i = 1, 100 do
    local co = coroutine.create(function()
        for j = 1, 3 do
            iterated = iterated + 1
            coroutine.yield(j)
        end
    end)
    coroutine.resume(co)
end
collectgarbage("collect")
collectgarbage("collect")
churn(100)
collectgarbage("collect")
assert_eq(iterated, 100, "an abandoned generator body stops at its yield")

-- 3) a tail-yield body owes nothing but its return, and recycling it must not leak values
local ok_values = true
for round = 1, 3 do
    for i = 1, 100 do
        local co = coroutine.create(function(n)
            return coroutine.yield(n * 2)
        end)
        local ok, v = coroutine.resume(co, i)
        if not ok or v ~= i * 2 then
            ok_values = false
        end
    end
    collectgarbage("collect")
end
assert_eq(ok_values, true, "tail-yield coroutines keep their yielded values across reuse")

-- 4) a to-be-closed local blocks the tail shortcut, and no coroutine may be closed twice by reuse
local closed = 0
for i = 1, 100 do
    local co = coroutine.create(function()
        local guard <close> = setmetatable({}, {
            __close = function() closed = closed + 1 end,
        })
        return coroutine.yield(1)
    end)
    coroutine.resume(co)
end
collectgarbage("collect")
collectgarbage("collect")
churn(100)
collectgarbage("collect")
assert_eq(closed <= 100, true, "an abandoned <close> coroutine is never closed twice")

-- 5) generators resumed to completion still sum correctly while fibers are recycled underneath
local sum = 0
for i = 1, 50 do
    local co = coroutine.wrap(function()
        for j = 1, 3 do
            coroutine.yield(j)
        end
        return 10
    end)
    for j = 1, 3 do
        sum = sum + co()
    end
    collectgarbage("collect")
end
assert_eq(sum, 50 * 6, "generator values survive fiber reuse")

-- 6) explicit close still works on a suspended tail-yield coroutine
local co = coroutine.create(function() return coroutine.yield(1) end)
coroutine.resume(co)
assert_eq(coroutine.close(co), true, "closing a suspended tail-yield coroutine succeeds")
assert_eq(coroutine.status(co), "dead", "a closed coroutine reports dead")

print_summary()
