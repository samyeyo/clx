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

print("----------------- Massive table literal")

-- Very large number of entries
local t = {}
for i = 1, 5000 do
    t[i] = i * 2
end
assert_eq(t[1], 2, "large table [1]")
assert_eq(t[2500], 5000, "large table [2500]")
assert_eq(t[5000], 10000, "large table [5000]")
assert_eq(#t, 5000, "large table length")

-- Mixed key types in large table
local big = {}
for i = 1, 1000 do
    big[i] = i
    big["k" .. i] = i * 10
end
assert_eq(big[500], 500, "big mixed [500]")
assert_eq(big["k500"], 5000, "big mixed ['k500']")
assert_eq(#big, 1000, "big mixed array length")

-- Nested massive tables
local nested = {}
for i = 1, 100 do
    nested[i] = {}
    for j = 1, 100 do
        nested[i][j] = (i - 1) * 100 + j
    end
end
assert_eq(nested[50][50], 4950, "nested massive [50][50]")
assert_eq(nested[100][100], 10000, "nested massive [100][100]")

-- Table with many string keys
local str_t = {}
for i = 1, 2000 do
    str_t["field_" .. tostring(i)] = i
end
assert_eq(str_t["field_1"], 1, "string key field_1")
assert_eq(str_t["field_2000"], 2000, "string key field_2000")

print_summary("COMPILER_KILLER_MASSIVE_TABLE")
