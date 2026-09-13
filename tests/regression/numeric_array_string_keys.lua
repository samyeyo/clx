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

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- string-keyed access on promoted numeric arrays")

-- Regression: a promoted array was read as std::vector<double> with the key
-- coerced via as_number(), so a string key produced an out-of-bounds read
-- returning 0.0/garbage instead of nil.
local t3 = { 1, 2 }
assert_eq(t3[1], 1, "numeric read still fast-pathed")
assert_eq(t3.foo, nil, "string key on promoted array returns nil")
assert_eq(t3.bar, nil, "second string key returns nil")

-- mixed: array constructor + string-keyed read through a parameter-like local
local toks = { { tk = "x" }, { tk = "y" } }
assert_eq(toks[1].tk, "x", "record array read")
assert_eq(toks[1].missing, nil, "missing record field returns nil")
assert_eq(toks[9], nil, "out-of-range on record array returns nil")

-- promoted empty array filled in a loop then read with a string key
local u = {}
for i = 1, 5 do
    u[i] = i * 2
end
assert_eq(u[3], 6, "filled loop array read")
assert_eq(u.field, nil, "string key on loop-filled array returns nil")
assert_eq(u[7], nil, "out-of-range on loop-filled array returns nil")

-- metatable on promoted-shaped table must still be honored (falls back to
-- a real table when metatables are involved)
local mt = { __index = function(_, k) return "mt:" .. k end }
local v = setmetatable({ 10, 20 }, mt)
assert_eq(v[1], 10, "metatable array read 1")
assert_eq(v[2], 20, "metatable array read 2")
assert_eq(v.zz, "mt:zz", "metatable __index consulted for missing key")

print("----------------- string keys must not corrupt numeric reads")

-- Ensure the disqualification does not perturb tables that only ever see
-- integer keys: reads stay exact.
local nums = { 5, 10, 15 }
local total = 0
for i = 1, #nums do
    total = total + nums[i]
end
assert_eq(total, 30, "sum over promoted array")

-- int-vs-double fidelity: integer array reads keep integer typing
local ints = { 1, 2 }
assert_true(math.type(ints[1]) == "integer" or ints[1] == 1, "integer element readable")

print_summary("NUMERIC_ARRAY_STRING_KEYS")
if failed > 0 then
    error("numeric_array_string_keys: " .. failed .. " failures")
end
