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

print("----------------- Operators & Precedence")

local a = 10
local b = 2

assert_eq(a + b * 3, 16, "math precedence mul add")

assert_eq((a + b) * 3, 36, "math precedence parentheses")

assert_eq(a - b - 1, 7, "math left associativity sub")

assert_eq(a / b * 2, 10, "math left associativity div mul")

local ineq_true = 0
if a ~= b then
    ineq_true = 1
else
    ineq_true = 0
end
assert_eq(ineq_true, 1, "inequality operator true")

local ineq_false = 0
if a ~= 10 then
    ineq_false = 1
else
    ineq_false = 0
end
assert_eq(ineq_false, 0, "inequality operator false")

local combo_val = 0
if a - b * 2 < a / b + 5 then
    combo_val = 1
else
    combo_val = 0
end
assert_eq(combo_val, 1, "math and relational combination")

print_summary("OPERATORS")