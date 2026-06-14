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

print("----------------- Control Flow")

local val_true = 0
if 1 == 1 then
    val_true = 1
else
    val_true = 2
end
assert_eq(val_true, 1, "if branch execution")

local val_false = 0
if 1 == 2 then
    val_false = 1
else
    val_false = 2
end
assert_eq(val_false, 2, "else branch execution")

local sum_for = 0
for i = 1, 5 do
    sum_for = sum_for + i
end
assert_eq(sum_for, 15, "numeric for loop")

local sum_while = 0
local j = 1
while j <= 5 do
    sum_while = sum_while + j
    j = j + 1
end
assert_eq(sum_while, 15, "while loop")

local sum_repeat = 0
local k = 1
repeat
    sum_repeat = sum_repeat + k
    k = k + 1
until k > 5
assert_eq(sum_repeat, 15, "repeat until loop")

print_summary("CONTROL FLOW")