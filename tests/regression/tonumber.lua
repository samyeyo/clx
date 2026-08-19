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

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- 1. Number passthrough")

assert_eq(tonumber(42), 42, "Integer passthrough")
assert_eq(tonumber(-7), -7, "Negative integer passthrough")
assert_eq(tonumber(3.5), 3.5, "Double passthrough")

print("\n----------------- 2. String conversion")

assert_eq(tonumber("42"), 42, "Decimal string")
assert_eq(tonumber("3.5"), 3.5, "Float string")
assert_eq(tonumber("-10"), -10, "Negative string")

assert_eq(tonumber("0x10"), 16, "Hex literal string without base")
assert_eq(tonumber("1e2"), 100.0, "Exponent string")

print("\n----------------- 3. Base argument")

assert_eq(tonumber("ff", 16), 255, "Hex lowercase base 16")
assert_eq(tonumber("FF", 16), 255, "Hex uppercase base 16")
assert_eq(tonumber("10", 2), 2, "Binary base 2")
assert_eq(tonumber("z", 36), 35, "Base 36 digit")
assert_eq(tonumber("-ff", 16), -255, "Negative with base")
assert_eq(tonumber("777", 8), 511, "Octal base 8")
assert_eq(tonumber("42", 10), 42, "Explicit base 10")
assert_eq(tonumber(" ff ", 16), 255, "Surrounding whitespace with base")
assert_eq(math.type(tonumber("ff", 16)), "integer", "Base conversion yields integer")

print("\n----------------- 4. Boundary values")

assert_eq(tonumber("7fffffffffffffff", 16), 9223372036854775807, "Int64 max via base 16")
assert_eq(tonumber("9223372036854775807"), 9223372036854775807.0, "Int64 max decimal string")

print("\n----------------- 5. Failure cases return nil")

assert_eq(tonumber("zz", 10), nil, "Invalid digits for base")
assert_eq(tonumber("", 16), nil, "Empty string with base")
assert_eq(tonumber("hello"), nil, "Non-numeric string")
assert_eq(tonumber(nil), nil, "Nil input")
assert_eq(tonumber(true), nil, "Boolean input")
assert_eq(tonumber({}), nil, "Table input")

print("\n----------------- 6. Invalid arguments raise errors")

assert_eq(pcall(tonumber, "ff", 1), false, "Base below 2 raises")
assert_eq(pcall(tonumber, "ff", 37), false, "Base above 36 raises")
assert_eq(pcall(tonumber, 42, 16), false, "Non-string value with base raises")

print_summary("TONUMBER")
