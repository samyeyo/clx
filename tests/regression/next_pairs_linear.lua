local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", tostring(actual))
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- next/pairs: O(1) key locate + iteration consistency")

local t = {}
for i = 1, 20 do
    t[i] = i * 10
end
t.a = 1
t.b = 2
t.c = 3
t[100] = "hundred"
t[10000] = "tenk"

local order = {}
local k = next(t)
while k ~= nil do
    order[#order + 1] = k
    k = next(t, k)
end
assert_eq(#order, 25, "full next() walk over mixed array+hash keys")

local sum = 0
for _, key in ipairs(order) do
    if type(key) == "number" then
        sum = sum + key
    end
end
assert_eq(sum, 210 + 100 + 10000, "numeric keys visited exactly once")

local seen_a, seen_100, seen_10000, seen_5 = false, false, false, false
for _, key in ipairs(order) do
    if key == "a" then
        seen_a = true
    end
    if key == 100 then
        seen_100 = true
    end
    if key == 10000 then
        seen_10000 = true
    end
    if key == 5 then
        seen_5 = true
    end
end
assert_eq(seen_a, true, "string key 'a' visited")
assert_eq(seen_100, true, "hash int key 100 visited")
assert_eq(seen_10000, true, "hash int key 10000 visited")
assert_eq(seen_5, true, "array key 5 visited")

t.b = nil
local order2 = {}
local k2 = next(t)
while k2 ~= nil do
    order2[#order2 + 1] = k2
    k2 = next(t, k2)
end
assert_eq(#order2, 24, "deleted key dropped from iteration")

local stepwise = true
for i = 1, #order2 - 1 do
    local nk = next(t, order2[i])
    if nk ~= order2[i + 1] then
        stepwise = false
    end
end
assert_eq(stepwise, true, "stepwise next(t, k) matches full-walk order")
assert_eq(next(t, order2[#order2]), nil, "next after last key returns nil")

local ok1, err1 = pcall(next, t, "zzz")
assert_eq(ok1, false, "absent string key raises")
assert_eq(type(err1) == "string" and err1:find("invalid key") ~= nil, true, "string key error message")

local ok2, err2 = pcall(next, t, 42)
assert_eq(ok2, false, "absent int key raises")
assert_eq(type(err2) == "string" and err2:find("invalid key") ~= nil, true, "int key error message")

local t2 = {}
table.insert(t2, 5)
table.insert(t2, 6)
table.insert(t2, 7)
assert_eq(next(t2, 3), nil, "next after last array slot")

local empty = {}
assert_eq(next(empty), nil, "next on empty table")

local big = {}
for i = 1, 49000 do
    big["k" .. i] = i
end
local count = 0
local kr, vr = next(big)
local big_ok = true
while kr ~= nil do
    count = count + 1
    if big[kr] ~= vr then
        big_ok = false
    end
    kr, vr = next(big, kr)
end
assert_eq(count, 49000, "next() walk over 49k-entry table (linear locate)")
assert_eq(big_ok, true, "every yielded pair round-trips")

local pr = 0
for _ in pairs(big) do
    pr = pr + 1
end
assert_eq(pr, 49000, "pairs() over 49k-entry table (linear locate)")

local mixed = {}
for i = 1, 40 do
    mixed[i] = i
    mixed["s" .. i] = i
end
local mc = 0
local mk, mv = next(mixed)
while mk ~= nil do
    assert_eq(mixed[mk], mv, "mixed table pair round-trip " .. tostring(mk))
    mc = mc + 1
    mk, mv = next(mixed, mk)
end
assert_eq(mc, 80, "mixed array+string walk")

print_summary("REGRESSION_NEXT_PAIRS_LINEAR")

if failed > 0 then os.exit(1) end
