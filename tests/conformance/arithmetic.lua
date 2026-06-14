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

print("----------------- Arithmetic & Variables")

local a = 10
local b = 5

assert_eq(a + b, 15, "addition")
assert_eq(a - b, 5, "subtraction")
assert_eq(a * b, 50, "multiplication")
assert_eq(a / b, 2, "division")

local c = a + b * 2
assert_eq(c, 20, "operator precedence")

global_var = 100
assert_eq(global_var, 100, "global variable storage")

assert_eq(3 & 5, 1, "Bitwise AND")
assert_eq(3 | 5, 7, "Bitwise OR")
assert_eq(3 ~ 5, 6, "Bitwise XOR")
assert_eq(~1, -2, "Bitwise NOT")
assert_eq(1 << 3, 8, "Left Shift")
assert_eq(16 >> 2, 4, "Right Shift")

print_summary("ARITHMETIC")