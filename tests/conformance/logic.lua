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

print("----------------- 1. Short-Circuit OR (Lazy Evaluation)")

local side_effect = 0
local function get_true()
    side_effect = side_effect + 1
    return true
end

local function get_20()
    side_effect = side_effect + 1
    return 20
end

local val1 = 10 or get_true()
assert_eq(val1, 10, "OR returns first value if truthy")
assert_eq(side_effect, 0, "OR short-circuits (no side effect)")

local val2 = nil or get_20()
assert_eq(val2, 20, "OR returns second value if first is nil")
assert_eq(side_effect, 1, "OR evaluated second operand")

local val3 = false or 30
assert_eq(val3, 30, "OR returns second value if first is false")

print("\n----------------- 2. Short-Circuit AND (Lazy Evaluation)")

side_effect = 0

local val4 = false and get_true()
assert_eq(val4, false, "AND returns first value if false")
assert_eq(side_effect, 0, "AND short-circuits (no side effect)")

local val5 = 0 and 50
assert_eq(val5, 50, "AND returns second value if first is truthy (0 is truthy)")

local val6 = 100 and get_20()
assert_eq(val6, 20, "AND returns second value if first is truthy")
assert_eq(side_effect, 1, "AND evaluated second operand")

print("\n----------------- 3. Complex Logic Chains")

local val7 = (nil or false or 50 or 60)
assert_eq(val7, 50, "Chain of ORs returns first truthy value")

local val8 = (10 and 20 and nil and 40)
assert_eq(val8, nil, "Chain of ANDs returns first falsy value")

print("\n----------------- 4. Bitwise Operators")

assert_eq(3 & 5, 1, "Bitwise AND (3 & 5)")
assert_eq(3 | 5, 7, "Bitwise OR (3 | 5)")
assert_eq(3 ~ 5, 6, "Bitwise XOR (3 ~ 5)")
assert_eq(10 << 2, 40, "Bitwise SHL (10 << 2)")
assert_eq(40 >> 2, 10, "Bitwise SHR (40 >> 2)")
assert_eq(~7, -8, "Bitwise NOT (~7)")

local flags = 0x01 | 0x02 | 0x04
assert_eq(flags & 0x02, 0x02, "Testing bitmask presence")
assert_eq(flags & 0x08, 0, "Testing bitmask absence")

print("\n----------------- 5. Precedence Tests")

local prec1 = 10 + 2 * 3
assert_eq(prec1, 16, "Math precedence (* over +)")

local prec2 = 10 | 2 & 1
assert_eq(prec2, 10, "Bitwise precedence (& over |)")

local prec3 = 10 + 2 << 1
assert_eq(prec3, 24, "Precedence: + over <<")

print_summary("LOGIC & BITWISE")