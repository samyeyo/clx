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

print("----------------- Strings")

local s1 = "hello"
local s2 = "world"
local s3 = "hello"

assert_eq(s1, s3, "string literal equality")

if s1 == s2 then
    assert_eq(1, 0, "string inequality fallback")
else
    assert_eq(1, 1, "string inequality fallback")
end

local t = {}
t["dynamic_key"] = "engine_test"
assert_eq(t.dynamic_key, "engine_test", "string as table key")

local s4 = t.dynamic_key
assert_eq(s4, "engine_test", "string assignment from table")

print_summary("STRINGS")