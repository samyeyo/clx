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

local function assert_nil(val, name)
    if val == nil then
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

print("----------------- Purity analysis: non-numeric keys disqualify arrays")

-- Bug: assigning with computed string keys (tbl['key_'..i]) used to
-- keep the array as pure-numeric, then crash on lookup (vector<double>
-- conversion on non-numeric key).

local function test_computed_string_keys()
    local t = {}
    for i = 1, 5 do
        t['key_' .. i] = i * 10
    end
    local sum = 0
    for i = 1, 5 do
        sum = sum + t['key_' .. i]
    end
    return sum
end
assert_eq(test_computed_string_keys(), 10 + 20 + 30 + 40 + 50, "computed string key store/load")

-- Mixed integer and string keys
local function test_mixed_key_types()
    local t = {}
    t[1] = 100
    t["label"] = "mixed"
    t[2] = 200
    return t[1] + t[2], t["label"]
end
local a, b = test_mixed_key_types()
assert_eq(a, 300, "mixed keys: integer access ok")
assert_eq(b, "mixed", "mixed keys: string access ok")

-- Table with string keys created dynamically
local function test_dynamic_string_table()
    local names = { "alice", "bob", "charlie" }
    local t = {}
    for idx, name in ipairs(names) do
        t[name] = idx
    end
    assert_eq(t["alice"], 1, "dynamic string key alice")
    assert_eq(t["bob"], 2, "dynamic string key bob")
    assert_eq(t["charlie"], 3, "dynamic string key charlie")
    assert_nil(t["dave"], "dynamic string key missing returns nil")
end
test_dynamic_string_table()

-- Table that looks like an array but has string keys too
local function test_hybrid_table()
    local t = { 10, 20, 30 }
    t["meta"] = "info"
    assert_eq(t[1], 10, "hybrid: array index 1")
    assert_eq(t[3], 30, "hybrid: array index 3")
    assert_eq(t["meta"], "info", "hybrid: string key")
    assert_eq(#t, 3, "hybrid: # returns array part length")
end
test_hybrid_table()

print_summary("REGRESSION_NON_NUMERIC_KEYS")
