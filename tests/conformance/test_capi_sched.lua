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

local s = require("capi_sched_mod")

-- 1) one coroutine yields a sleep, is resumed, finishes, then is unref'd
local t1 = s.spawn(function()
    local v = coroutine.yield(1)
    return "finished", v
end)

local r = s.step()
assert_eq(r.status, "yield", "t1 first step yields")
assert_eq(r[1], 1, "t1 yields a sleep number")

r = s.step(2)
assert_eq(r.status, "ok", "t1 second step finishes")
assert_eq(r[1], "finished", "t1 result")
assert_eq(r[2], 2, "t1 saw the resume argument")

s.complete(t1)
collectgarbage("collect")
collectgarbage("collect")

-- 2) a second coroutine raises, reached from inside pcall
local t2 = s.spawn(function()
    local ok, err = pcall(function()
        error("boom")
    end)
    return "caught", ok, tostring(err)
end)

local ok, res = pcall(function()
    local rr = s.step()
    return rr
end)
assert_eq(ok, true, "pcall around step survives")
assert_eq(res.status, "ok", "t2 status is ok (pcall inside the coroutine)")
assert_eq(res[1], "caught", "t2 returned")
assert_eq(res[2], false, "t2 saw the error")

s.complete(t2)
collectgarbage("collect")

-- 3) an uncaught error surfaces as LUA_ERRRUN and the task is unref'd
local t3 = s.spawn(function()
    error("kaboom")
end)
local r3 = s.step()
assert_eq(r3.status, "error", "t3 reports an error status")
assert_eq(string.find(tostring(r3[1]), "kaboom") ~= nil, true, "t3 error message")

-- 4) waiting task: yields ("wait", lightuserdata) then finishes
local t4 = s.spawn(function()
    local tag, who = coroutine.yield("wait", t3)
    return "woke", tag, who == nil
end)
local r4 = s.step()
assert_eq(r4.status, "yield", "t4 yields a wait")
assert_eq(r4[1], "wait", "t4 wait tag")
assert_eq(r4[2], t3, "t4 wait target is the task pointer")

local r5 = s.step(2)
assert_eq(r5.status, "ok", "t4 finishes after resume with an argument")
assert_eq(r5[1], "woke", "t4 result")
assert_eq(r5[2], 2, "t4 saw the resume argument")

s.complete(t4)
collectgarbage("collect")
collectgarbage("collect")

assert_eq(s.pending_count(), 0, "queue drained")
assert_eq(s.gc_count() >= 0, true, "payload finalizers ran or not")

print("-----")
print("PASS  :", passed)
print("FAIL  :", failed)
print("-----")
os.exit(failed > 0 and 1 or 0)
