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

local function assert_true(val, name)
    if val then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name)
    end
end

local function assert_error_contains(fn, name)
    local ok, err = pcall(fn)
    if not ok then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected error, got success with:", err)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- nil-base table access raises instead of crashing")

-- Regression: fast-path chains emitted static_cast<LTable*>(v.as_pointer())
-- without a nil check, segfaulting when an intermediate field was nil.
local ps = { tokens = { { tk = "a" }, { tk = "b" } } }
local f = function(p, i)
    return p.tokens[i].tk
end
assert_eq(f(ps, 1), "a", "in-range chain read works")
assert_eq(f(ps, 2), "b", "in-range chain read 2 works")

-- nil field inside the chain: must raise "attempt to index a nil value"
local g = function(p, i)
    return p.tokens[i].zzz -- tokens[i] exists but has no .zzz; reading .deeper would be the crash
end
assert_eq(g(ps, 1), nil, "missing field returns nil")

local h = function(p, i)
    return p.missing[i].tk -- p.missing is nil -> indexing nil must raise
end
assert_error_contains(function() h(ps, 1) end, "nil intermediate field raises")

local h2 = function(p)
    return p.tokens[9].tk -- tokens[9] is nil -> indexing nil must raise
end
assert_error_contains(function() h2(ps) end, "nil out-of-range chain element raises")

local h3 = function(p)
    return p.tokens[1](ps) -- tokens[1] is a table, not callable; separate path
end
assert_error_contains(function() h3(ps) end, "calling non-function still raises")

-- indexing a nil local / global directly
local nilv
local h4 = function()
    return nilv.field
end
assert_error_contains(function() h4() end, "indexing nil local raises")

-- indexing a string base
local h5 = function()
    local s = "hello"
    return s[1]
end
assert_error_contains(function() h5() end, "indexing string with integer raises")

print("----------------- out-of-range reads on numeric arrays return nil")

-- Regression: promoted arrays were read as bare std::vector<double> with no
-- bounds check; t[10] on {1,2,3} read out of bounds and returned 0.0/garbage.
local t = { 1, 2, 3 }
assert_eq(t[1], 1, "array read 1")
assert_eq(t[2], 2, "array read 2")
assert_eq(t[3], 3, "array read 3")
assert_eq(t[10], nil, "out-of-range literal read returns nil")

local sum = 0
for i = 1, 3 do
    sum = sum + t[i]
end
assert_eq(sum, 6, "in-range loop reads still work")

-- integer-key read just past the end
assert_eq(t[4], nil, "read just past end returns nil")

print("----------------- hole-creating writes keep Lua semantics")

-- Regression: v[4] = 40 on a 2-element promoted array wrote through the vector
-- (creating a hole and a wrong length). After the fix the array is left as a
-- real table when writes exceed its known bound, so Lua semantics hold.
local v = { 1, 2 }
v[4] = 40
assert_eq(v[4], 40, "out-of-range write visible")
assert_eq(v[3], nil, "hole reads as nil after out-of-range write")
assert_eq(#v, 2, "length border stays at the hole (Lua semantics)")

-- nil assignment extends but leaves a nil hole
local w = { 1 }
w[3] = nil
assert_eq(#w, 1, "nil write does not extend length")

print_summary("FASTPATH_INDEX_SAFETY")
if failed > 0 then
    error("fastpath_index_safety: " .. failed .. " failures")
end
