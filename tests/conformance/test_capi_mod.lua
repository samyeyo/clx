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

print("----------------- Lua C API: continuations in lua_pcallk / lua_callk")

local m = require("capi_mod")

local LUA_OK = 0
local LUA_YIELD = 1
local LUA_ERRRUN = 2
local YIELD_ERR = "attempt to yield across a C-call boundary"

-- (a) callee yields once via Lua coroutine.yield, then returns
m.reset_counts()
local co_a = coroutine.create(function()
    return m.mypall(function()
        local y = m.isyieldable()
        coroutine.yield(7)
        return y, 5, 6
    end)
end)
local aok1, ay = coroutine.resume(co_a)
assert_eq(aok1, true, "a) first resume succeeds")
assert_eq(ay, 7, "a) yielded through lua_pcallk")
assert_eq(coroutine.status(co_a), "suspended", "a) suspended inside lua_pcallk")
local aok2, ar1, ar2, ar3, ar4 = coroutine.resume(co_a)
assert_eq(aok2, true, "a) second resume succeeds")
assert_eq(ar1, true, "a) pcall success flag")
assert_eq(ar2, 1, "a) lua_isyieldable inside the callee")
assert_eq(ar3, 5, "a) result 1")
assert_eq(ar4, 6, "a) result 2")
local afc, afs = m.counts()
assert_eq(afc, 1, "a) finish runs exactly once")
assert_eq(afs, LUA_YIELD, "a) finish status is LUA_YIELD")
assert_eq(coroutine.status(co_a), "dead", "a) coroutine finished")

-- (b) callee yields via lua_yield from a C function
m.reset_counts()
local co_b = coroutine.create(function() return m.mypall(m.c_yielder) end)
local bok1, by = coroutine.resume(co_b)
assert_eq(bok1, true, "b) first resume succeeds")
assert_eq(by, 42, "b) yielded 42 from lua_yield")
local bok2, br1, br2 = coroutine.resume(co_b, 55)
assert_eq(bok2, true, "b) second resume succeeds")
assert_eq(br1, true, "b) pcall success flag")
assert_eq(br2, 55, "b) resume value became the callee's result")
local bfc, bfs = m.counts()
assert_eq(bfc, 1, "b) finish runs exactly once")
assert_eq(bfs, LUA_YIELD, "b) finish status is LUA_YIELD")
assert_eq(coroutine.status(co_b), "dead", "b) coroutine finished")

-- (c) callee yields, then errors after resume
m.reset_counts()
local co_c = coroutine.create(function()
    return m.mypall(function()
        coroutine.yield(1)
        error("boom")
    end)
end)
local cok1, cy = coroutine.resume(co_c)
assert_eq(cok1, true, "c) first resume succeeds")
assert_eq(cy, 1, "c) yielded once")
local cres = { coroutine.resume(co_c) }
assert_eq(cres[1], true, "c) second resume succeeds")
assert_eq(cres[2], false, "c) pcall reports failure")
assert_eq(string.find(cres[3] or "", "boom") ~= nil, true, "c) error message propagated")
local cfc, cfs = m.counts()
assert_eq(cfc, 1, "c) finish runs exactly once")
assert_eq(cfs, LUA_ERRRUN, "c) finish status is LUA_ERRRUN")
assert_eq(coroutine.status(co_c), "dead", "c) coroutine finished")

-- (d) callee never yields: the normal path calls finish exactly once
m.reset_counts()
local d1, d2, d3 = m.mypall(function() return 10, 20 end)
assert_eq(d1, true, "d) pcall success flag")
assert_eq(d2, 10, "d) result 1")
assert_eq(d3, 20, "d) result 2")
local dfc, dfs = m.counts()
assert_eq(dfc, 1, "d) finish runs exactly once")
assert_eq(dfs, LUA_OK, "d) finish status is LUA_OK on the normal path")

-- (e) k == NULL and the callee yields: refused with the C-call boundary error
m.reset_counts()
local co_e = coroutine.create(function() return m.run_pcall(false) end)
local eok, est, eyld, emsg, ekc = coroutine.resume(co_e)
assert_eq(eok, true, "e) resume succeeds")
assert_eq(est, LUA_ERRRUN, "e) lua_pcallk returns LUA_ERRRUN")
assert_eq(eyld, 0, "e) lua_isyieldable == 0 with k == NULL")
assert_eq(emsg, YIELD_ERR, "e) error message")
assert_eq(ekc, 0, "e) continuation k was not invoked")
assert_eq(coroutine.status(co_e), "dead", "e) no suspension happened")

-- (f) lua_callk with k: the continuation runs with LUA_YIELD after the yield
m.reset_counts()
local co_f = coroutine.create(function()
    return m.run_callk(function()
        coroutine.yield(9)
        return 3
    end)
end)
local fok1, fy = coroutine.resume(co_f)
assert_eq(fok1, true, "f) first resume succeeds")
assert_eq(fy, 9, "f) yielded through lua_callk")
local fok2, fv = coroutine.resume(co_f)
assert_eq(fok2, true, "f) second resume succeeds")
assert_eq(fv, nil, "f) the C function returned k's results (none)")
local _, _, fcc, fcs = m.counts()
assert_eq(fcc, 1, "f) k called exactly once")
assert_eq(fcs, LUA_YIELD, "f) k status is LUA_YIELD")
assert_eq(coroutine.status(co_f), "dead", "f) coroutine finished")
m.reset_counts()
local f2 = m.run_callk(function() return 42 end)
local _, _, fcc2 = m.counts()
assert_eq(f2, 42, "f) no yield: results returned normally")
assert_eq(fcc2, 0, "f) no yield: k not called")

-- (g) mypall nested inside mypall, inner callee yields
m.reset_counts()
local co_g = coroutine.create(function()
    return m.mypall(function()
        return m.mypall(function()
            coroutine.yield(3)
            return 7
        end)
    end)
end)
local gok1, gy = coroutine.resume(co_g)
assert_eq(gok1, true, "g) first resume succeeds")
assert_eq(gy, 3, "g) inner callee yielded")
local gres = { coroutine.resume(co_g) }
assert_eq(gres[1], true, "g) second resume succeeds")
assert_eq(gres[2], true, "g) outer pcall success flag")
assert_eq(gres[3], true, "g) inner pcall success flag")
assert_eq(gres[4], 7, "g) inner result")
local gfc, gfs = m.counts()
assert_eq(gfc, 2, "g) each finish runs once")
assert_eq(gfs, LUA_YIELD, "g) finish status is LUA_YIELD")
assert_eq(coroutine.status(co_g), "dead", "g) coroutine finished")

-- (h) close a coroutine suspended inside a yieldable lua_pcallk
m.reset_counts()
local co_h = coroutine.create(function() return m.run_pcall(true) end)
local hok1, hv = coroutine.resume(co_h)
assert_eq(hok1, true, "h) first resume succeeds")
assert_eq(hv, 42, "h) suspended inside yieldable lua_pcallk")
assert_eq(m.thread_isyieldable(co_h), 1, "h) pcall_depth == 0 while suspended")
assert_eq(coroutine.close(co_h), true, "h) close completes the thread")
assert_eq(coroutine.status(co_h), "dead", "h) thread ends DEAD")
assert_eq(m.thread_isyieldable(co_h), 1, "h) pcall_depth back to 0 after close")
local _, _, _, _, hkc = m.counts()
assert_eq(hkc, 0, "h) continuation k was not invoked on close")

print_summary("LUA_CAPI_PCALLK_YIELD")
os.exit(failed > 0 and 1 or 0)
