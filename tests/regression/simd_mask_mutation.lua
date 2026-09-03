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

print("----------------- table mutation at SIMD-width boundaries (rawlen/next NEON masks)")

-- Fill exactly to sizes around the 16-byte SIMD window: nil slots land on both
-- even and odd byte indices, exposing masks that drop half the lanes.
local sizes = { 1, 2, 7, 8, 9, 15, 16, 17, 23, 31, 32, 33 }

for _, n in ipairs(sizes) do
    -- table.remove() on the last element must return that element, not nil
    local t = {}
    for i = 1, n do
        t[i] = i * 100
    end
    local v = table.remove(t)
    assert_eq(v, n * 100, "remove last of " .. n)
    assert_eq(#t, n - 1, "len after remove of " .. n)
    assert_eq(t[n], nil, "slot cleared after remove of " .. n)

    -- table.insert + remove round trip on every position
    local u = {}
    for i = 1, n do
        table.insert(u, i)
    end
    local mid = math.max(1, math.floor(n / 2))
    table.remove(u, mid)
    assert_eq(#u, n - 1, "len after remove(pos) of " .. n)
    for i = 1, n - 1 do
        local expected = i < mid and i or i + 1
        assert_eq(u[i], expected, "shifted elem " .. i .. "/" .. n)
    end

    -- generic for over a dynamically mutated table must visit every live slot
    local w = {}
    for i = 1, n do
        w[i] = i
    end
    table.insert(w, 1000)
    table.remove(w, 1)
    local sum = 0
    local count = 0
    for _, val in ipairs(w) do
        sum = sum + val
        count = count + 1
    end
    assert_eq(count, n, "ipairs visits all " .. n)
    assert_eq(sum, n * (n + 1) / 2 - 1 + 1000, "sum after mutate " .. n)

    -- pairs() via next() must also see the rebuilt array
    local pc = 0
    for _ in pairs(w) do
        pc = pc + 1
    end
    assert_eq(pc, n, "pairs count after mutate " .. n)

    -- nil hole at odd SIMD index must terminate rawlen
    local h = {}
    for i = 1, n + 1 do
        h[i] = i
    end
    h[n + 1] = nil
    assert_eq(#h, n, "hole len " .. n)
end

-- mixed types crossing the SIMD window (type tags are the scanned bytes)
local m = {}
for i = 1, 20 do
    m[i] = i % 3 == 0 and tostring(i) or i
end
assert_eq(#m, 20, "mixed types len 20")
m[19] = nil
m[20] = nil
assert_eq(#m, 18, "mixed types len 18 after holes")

-- 16-byte exact window: all 16 slots non-nil, then kill odd slot 15 (byte 14) and even slot 16 (byte 15)
local e = {}
for i = 1, 16 do
    e[i] = i
end
assert_eq(#e, 16, "full window len 16")
e[15] = nil
assert_eq(#e, 14, "nil at byte 14 (even idx) seen")
local e2 = {}
for i = 1, 16 do
    e2[i] = i
end
e2[16] = nil
assert_eq(#e2, 15, "nil at byte 15 (odd idx, previously dropped by vmovn)")

-- find_first_nonnil: first live element after a leading run of holes at odd/even offsets
local f = {}
f[2] = "a"
assert_eq(next(f), 2, "next skips leading nil at odd byte 1")
local f2 = {}
f2[1] = nil
f2[3] = "b"
assert_eq((next(f2, 2)), 3, "next from hole finds odd-byte slot")

-- string.find pattern-class scan (clx_find_byte_of_set had the same narrowing bug)
assert_eq(("abc"):find("[%d]"), nil, "pattern class no match")
assert_eq(("ab1c"):find("%d"), 3, "pattern class match at odd byte 2")
local long = string.rep("x", 23) .. "."
assert_eq(long:find("%."), 24, "pattern meta at odd byte 23")

print_summary("table mutation SIMD boundary")
if failed > 0 then
    error("REGRESSION: " .. failed .. " failures")
end
