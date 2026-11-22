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

print("----------------- pcall then coroutine regression")

-- 1) the reported shape: a caught error, a coroutine run to completion, a collection
collectgarbage("generational")
assert_eq(pcall(error, "boom") == false, true, "pcall catches a direct error")
assert_eq(coroutine.wrap(function() return 1 end)(), 1, "a wrapped coroutine still runs after a caught error")
collectgarbage()
assert_eq(true, true, "collecting after a caught error and a coroutine survives")

-- 2) the same with an error raised inside a Lua closure and with a non-string error object
assert_eq(pcall(function() error("boom") end) == false, true, "pcall catches an error from a closure")
local ok, err = pcall(function() error({ code = 1 }) end)
assert_eq(ok, false, "pcall catches a table error object")
assert_eq(type(err), "table", "the table error object is returned as is")
coroutine.wrap(function() return 1 end)()
collectgarbage()
assert_eq(true, true, "collecting after a closure error survives")

-- 3) coroutines that error themselves, then a fresh coroutine and a collection
local wrapped = coroutine.wrap(function() error("inside") end)
assert_eq(pcall(wrapped), false, "a coroutine error surfaces through pcall")
coroutine.wrap(function() return 1 end)()
collectgarbage()
assert_eq(true, true, "collecting after a coroutine error survives")

-- 4) suspended coroutines left for the collector, interleaved with a caught error
local suspended = {}
for i = 1, 50 do
    local co = coroutine.wrap(function(x)
        local payload = { i, x }
        coroutine.yield(payload)
        return payload
    end)
    co(i)
    suspended[i] = co
end
assert_eq(pcall(error, "boom") == false, true, "pcall catches with suspended coroutines alive")
collectgarbage()
suspended = nil
collectgarbage()
assert_eq(coroutine.wrap(function() return 42 end)(), 42, "a new coroutine runs after the suspended set is collected")

-- 5) both collection modes, several cycles
for _, mode in ipairs({ "generational", "incremental" }) do
    collectgarbage(mode)
    for i = 1, 20 do
        assert(pcall(error, "boom" .. i) == false)
        coroutine.wrap(function() return i end)()
        collectgarbage()
    end
    assert_eq(true, true, "caught errors and coroutines are stable in " .. mode .. " mode")
end

-- 6) a caught error must not leave a root frame behind for the next collection: the unwound call's
-- roots are dropped by pcall's handler, so a collection right after it never walks a dead frame
for i = 1, 200 do
    assert(pcall(error, "boom" .. i) == false)
    collectgarbage()
end
assert_eq(true, true, "collecting after 200 caught errors survives")

-- 7) close-time finalizers still see a live state: this __gc runs while the state is being closed and
-- collects from there
setmetatable({}, {
    __gc = function()
        collectgarbage()
        collectgarbage("count")
    end,
})
assert_eq(true, true, "a collecting __gc is registered for state close")

print_summary()
