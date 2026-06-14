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

print("----------------- CacheSlot: only for fixed string literal keys")

-- Bug: CacheSlot wrapped computed-key reads (snprintf/concat results)
-- in cache slots that only checked the table pointer, not the key.
-- This returned stale results for varying keys.

local function test_computed_key_get()
    local t = { a = 10, b = 20, c = 30 }
    local keys = { "a", "b", "c" }
    local sum = 0
    for i = 1, 3 do
        -- computed key via table lookup
        sum = sum + t[keys[i]]
    end
    return sum
end
assert_eq(test_computed_key_get(), 60, "computed key via table lookup")

local function test_concat_key_get()
    local t = {}
    for i = 1, 5 do
        t["item" .. i] = i
    end
    local sum = 0
    for i = 1, 5 do
        sum = sum + t["item" .. i]
    end
    return sum
end
assert_eq(test_concat_key_get(), 15, "concat key store/load")

-- Varying computed key in a loop
local function test_varying_concat_key()
    local dict = { a1 = 1, b2 = 2, c3 = 3, d4 = 4 }
    local prefixes = { "a", "b", "c", "d" }
    local pair = {}
    for i = 1, 4 do
        pair[prefixes[i]] = dict[prefixes[i] .. i]
    end
    assert_eq(pair["a"], 1, "pair a -> 1")
    assert_eq(pair["b"], 2, "pair b -> 2")
    assert_eq(pair["c"], 3, "pair c -> 3")
    assert_eq(pair["d"], 4, "pair d -> 4")
end
test_varying_concat_key()

-- Computed key with snprintf-style building
local function test_built_key()
    local cache = {}
    for i = 1, 10 do
        local k = "slot_" .. i
        cache[k] = i * i
    end
    local sum = 0
    for i = 1, 10 do
        local k = "slot_" .. i
        sum = sum + cache[k]
    end
    -- sum of squares 1..10 = 385
    assert_eq(sum, 385, "built key sum of squares")
end
test_built_key()

print_summary("REGRESSION_CACHESLOT")
