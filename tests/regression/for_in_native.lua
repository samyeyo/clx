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

print("----------------- For-in vars not treated as native numbers")

-- Bug: for-in loop variables were incorrectly added to g_native_numbers
-- by previous numeric for loops, causing native double ops on non-numbers.

-- Mix numeric for and for-in
local function test_mixed_loops()
    local t = { a = 10, b = 20, c = 30 }
    local sum = 0
    for i = 1, 3 do
        sum = sum + i
    end
    for k, v in pairs(t) do
        sum = sum + v
    end
    return sum
end
assert_eq(test_mixed_loops(), 6 + 60, "numeric for then for-in does not corrupt vars")

-- For-in with string keys and non-numeric values
local function test_for_in_strings()
    local lookup = { x = "hello", y = "world" }
    local out = ""
    for k, v in pairs(lookup) do
        out = out .. v
    end
    return out
end
-- order of pairs is undefined; just check both values present
local res_s = test_for_in_strings()
assert_true(res_s == "helloworld" or res_s == "worldhello", "for-in with string values works")

-- For-in with table values
local function test_for_in_tables()
    local registry = { a = { id = 1 }, b = { id = 2 } }
    local sum = 0
    for k, v in pairs(registry) do
        sum = sum + v.id
    end
    return sum
end
assert_eq(test_for_in_tables(), 3, "for-in with table values works")

-- For-in with nil gaps (pairs skips nils)
local function test_for_in_sparse()
    local t = {}
    t[1] = 10
    t[3] = 30
    local sum = 0
    for k, v in pairs(t) do
        sum = sum + v
    end
    return sum
end
assert_eq(test_for_in_sparse(), 40, "for-in sparse table skips nil gaps")

print_summary("REGRESSION_FOR_IN_NATIVE")
