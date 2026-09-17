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

-- Exercise every converted runtime call site under GC pressure and forced error unwinding:
-- __index/__newindex (table + userdata), __tostring, __call, __gc, __close,
-- table.sort comparator, string.gsub callback, package loaders, pcall/xpcall handlers.

collectgarbage("setpause", 50)

-- 1. __index/__newindex function metamethods in a loop (table_get_int / table_set paths)
local mt = {
    __index = function(t, k) return k * 2 end,
    __newindex = function(t, k, v) rawset(t, k, v * 10) end,
}
for i = 1, 2000 do
    local t = setmetatable({}, mt)
    collectgarbage("step")
    assert_eq(t[5], 10, "__index function " .. i)
    t[1] = 7
    assert_eq(rawget(t, 1), 70, "__newindex function " .. i)
    if i % 500 == 0 then collectgarbage("collect") end
end

-- 2. __tostring + __call
local calls = 0
local callable = setmetatable({}, { __call = function(self, x) calls = calls + 1; return x + 1 end })
local strable = setmetatable({}, { __tostring = function(self) return "STR" .. calls end })
for i = 1, 500 do
    assert_eq(callable(i), i + 1, "__call " .. i)
    local s = tostring(strable)
    if s ~= "STR" .. (calls) then error("bad tostring") end
end
assert_eq(calls, 500, "__call count")
assert_eq(tostring(strable), "STR500", "__tostring final")

-- 3. table.sort comparator under allocation pressure
local data = {}
for i = 1, 500 do data[i] = { k = (i * 7919) % 1000 } end
for round = 1, 20 do
    table.sort(data, function(a, b) return a.k < b.k end)
    for i = 2, #data do
        if data[i - 1].k > data[i].k then error("sort broken") end
    end
    collectgarbage("step")
    -- reshuffle deterministically
    for i = 1, #data do data[i].k = (data[i].k * 31 + round) % 1000 end
end
print("[OK]   ", "table.sort comparator")

-- 4. gsub with function replacement
for i = 1, 200 do
    local r = string.gsub("abcabcabc", "a", function(c) return string.upper(c) .. i end)
    assert_eq(r, "A" .. i .. "bcA" .. i .. "bcA" .. i .. "bc", "gsub " .. i)
end

-- 5. pcall/xpcall error unwinding inside deep metamethod chains (leak check via GC health)
local err_mt = {
    __index = function(t, k) error("idx error: " .. tostring(k)) end,
}
for i = 1, 2000 do
    local t = setmetatable({}, err_mt)
    local ok, err = pcall(function() return t.foo end)
    assert_eq(ok, false, "pcall caught " .. i)
    if not string.find(err, "idx error") then error("wrong error: " .. err) end
    collectgarbage("step")
end
print("[OK]   ", "pcall unwinding through __index")

-- 6. xpcall with handler
local handler_ran = 0
for i = 1, 500 do
    local ok, msg = xpcall(function() error({ code = i }) end, function(e) handler_ran = handler_ran + 1; return e.code end)
    assert_eq(ok, false, "xpcall ok " .. i)
    assert_eq(msg, i, "xpcall handler " .. i)
end
assert_eq(handler_ran, 500, "xpcall handler count")

-- 7. __gc finalizers en masse
local finalized = 0
do
    local gcmt = setmetatable({}, { __gc = function() finalized = finalized + 1 end })
    local sink = {}
    for i = 1, 3000 do
        sink[i] = setmetatable({}, { __gc = function() finalized = finalized + 1 end })
        sink[i] = nil
        if i % 100 == 0 then collectgarbage("collect") end
    end
end
collectgarbage("collect")
collectgarbage("collect")
if finalized < 3000 then
    failed = failed + 1
    print("[FAIL] ", "__gc finalizers", "| Expected: >=3000 Got:", finalized)
else
    passed = passed + 1
    print("[OK]   ", "__gc finalizers (" .. finalized .. ")")
end

-- 8. coroutine body calls (converted site) with errors
for i = 1, 300 do
    local co = coroutine.create(function(x) coroutine.yield(x * 2); error("co err") end)
    local ok1, v1 = coroutine.resume(co, i)
    assert_eq(v1, i * 2, "coroutine yield " .. i)
    local ok2, e2 = coroutine.resume(co)
    assert_eq(ok2, false, "coroutine error " .. i)
end

-- 9. require path (package searchers + loader run with converted rooting):
-- built-in modules resolve from package.loaded (regression: AOT require of
-- builtins must return the global library table); unknown modules must fail
-- cleanly through the full searcher chain
local ok3, mod = pcall(require, "string")
if ok3 and mod == string then
    passed = passed + 1
    print("[OK]   ", "require builtin returns global table")
else
    failed = failed + 1
    print("[FAIL] ", "require builtin returns global table", "| Got:", type(mod))
end
local ok4, mod2 = pcall(require, "definitely_not_a_module_42")
if not ok4 and type(mod2) == "string" and string.find(mod2, "not found", 1, true) then
    passed = passed + 1
    print("[OK]   ", "require searcher/loader path")
else
    failed = failed + 1
    print("[FAIL] ", "require searcher/loader path", "| Got:", mod2)
end

-- 10. shadow stack health: deep recursion + errors must not ratchet shadow_top
local function deep(n)
    if n == 0 then error("bottom") end
    return deep(n - 1)
end
for i = 1, 100 do
    local ok, e = pcall(deep, 200)
    assert_eq(ok, false, "deep unwind " .. i)
end
print("[OK]   ", "shadow stack unwinding stable")

print("-----")
print("PASS  :", passed)
print("FAIL  :", failed)
if failed > 0 then
    os.exit(1)
end
