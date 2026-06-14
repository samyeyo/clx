local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
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

print("----------------- Relational Conditions")

local a = 10
local b = 20
local c = 10

if a < b then
    assert_eq(1, 1, "less than")
else
    assert_eq(0, 1, "less than")
end

if b > a then
    assert_eq(1, 1, "greater than")
else
    assert_eq(0, 1, "greater than")
end

if a <= c then
    assert_eq(1, 1, "less equal")
else
    assert_eq(0, 1, "less equal")
end

if b >= a then
    assert_eq(1, 1, "greater equal")
else
    assert_eq(0, 1, "greater equal")
end

if a == c then
    assert_eq(1, 1, "equal")
else
    assert_eq(0, 1, "equal")
end

if a == b then
    assert_eq(1, 0, "inequality fallback")
else
    assert_eq(1, 1, "inequality fallback")
end

print_summary("CONDITIONS")