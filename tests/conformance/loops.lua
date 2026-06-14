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

local function print_summary()
    print("-----")
    print("SUITE :", "LOOPS")
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- Loops")

local i = 0
local sum_while = 0

while i < 5 do
    i = i + 1
    sum_while = sum_while + i
end

assert_eq(i, 5, "while loop counter")
assert_eq(sum_while, 15, "while loop sum")

local j = 10
local sum_repeat = 0

repeat
    sum_repeat = sum_repeat + j
    j = j - 1
until j == 0

assert_eq(j, 0, "repeat until counter")
assert_eq(sum_repeat, 55, "repeat until sum")

local sum_for_default = 0

for k = 1, 5 do
    sum_for_default = sum_for_default + k
end

assert_eq(sum_for_default, 15, "numeric for default step")

local sum_for_step = 0

for l = 2, 10, 2 do
    sum_for_step = sum_for_step + l
end

assert_eq(sum_for_step, 30, "numeric for positive step")

local sum_for_negative = 0

for m = 10, 1, -2 do
    sum_for_negative = sum_for_negative + m
end

assert_eq(sum_for_negative, 30, "numeric for negative step")

local nested_sum = 0

for x = 1, 3 do
    for y = 1, 3 do
        nested_sum = nested_sum + (x * y)
    end
end

-- x=1: (1*1) + (1*2) + (1*3) = 6
-- x=2: (2*1) + (2*2) + (2*3) = 12
-- x=3: (3*1) + (3*2) + (3*3) = 18
-- Total: 6 + 12 + 18 = 36
assert_eq(nested_sum, 36, "nested for loops")

print_summary()
