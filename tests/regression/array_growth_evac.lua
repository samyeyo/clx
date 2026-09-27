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

print("----------------- array growth hash evacuation (park + tombstone)")

local t = {}
t[1] = 0
local r = 10
while r > 1 do
    t[r] = r
    r = r - 1
end
t[9] = 5
assert_eq(t[10], 10, "t[10] survives growth at t[9]")
assert_eq(t[9], 5, "t[9] holds new value")
local n = 0
local seen9 = 0
for k, v in pairs(t) do
    n = n + 1
    if k == 9 then seen9 = seen9 + 1 end
    if n > 50 then break end
end
assert_eq(n, 10, "pairs yields exactly 10 keys")
assert_eq(seen9, 1, "key 9 appears exactly once in pairs")

local d = {}
d[1] = 0
r = 10
while r > 1 do
    d[r] = r
    r = r - 1
end
d[9] = d[9] - 1
d[9] = d[9] - 1
assert_eq(d[10], 10, "d[10] intact after two decrements")
assert_eq(d[9], 7, "d[9] decremented twice")

local m = {}
m[1] = 0
m[10] = 10
m[12] = 12
r = 8
while r > 1 do
    m[r] = r
    r = r - 1
end
m[9] = 55
assert_eq(m[10], 10, "parked m[10] visible")
assert_eq(m[12], 12, "parked m[12] visible past gap")
assert_eq(m[9], 55, "m[9] new value")
local n2 = 0
for k, v in pairs(m) do
    n2 = n2 + 1
    if n2 > 50 then break end
end
assert_eq(n2, 11, "pairs after multi-park")

local f = {}
f[1] = 1
f[100] = 100
for i = 2, 24 do
    f[i] = i
end
assert_eq(f[100], 100, "far hash key survives growths")
assert_eq(f[24], 24, "array fill complete")

local dt = {}
dt[1] = 0
dt[10] = 10
r = 8
while r > 2 do
    dt[r] = r
    r = r - 1
end
dt[9.0] = 77
assert_eq(dt[10], 10, "double-key growth: t[10] survives")
assert_eq(dt[9], 77, "double-key growth: t[9] new value")

print_summary("REGRESSION_ARRAY_GROWTH_EVAC")

if failed > 0 then os.exit(1) end
