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

print("----------------- Deeply nested binary operations")

-- Stress parser/optimizer with deep expression trees
local function deep_arith(n)
    local r = 1
    for _ = 1, n do
        r = r + r * 2 - r / 2 + r % 3
    end
    return r
end
assert_eq(type(deep_arith(1000)), "number", "deep arithmetic returns number")

-- Deep nested ternary-style via and/or
local function deep_logic(n)
    local x = 1
    for _ = 1, n do
        x = (x > 0 and x < 100) and (x + 1) or 0
    end
    return x
end
assert_eq(deep_logic(50), 51, "deep logic chains 50 iters")

-- Nested bitops (fixed mask width)
local function deep_bitops()
    local x = 0xFF
    for _ = 1, 8 do
        x = ((x << 1) | (x >> 7)) & 0xFF
    end
    return x
end
assert_eq(deep_bitops(), 0xFF, "deep bitwise rotation 8 rounds returns original")

-- Deep concatenation chain
local function deep_concat(n)
    local s = ""
    for i = 1, n do
        s = s .. "x" .. tostring(i) .. "y"
    end
    return #s
end
-- length = sum of (2 + digits(i)) for i = 1..100 = 200 + (9*1 + 90*2 + 1*3) = 200 + 192 = 392
assert_eq(deep_concat(100), 392, "deep concat 100 iters length")

-- Nested function calls with arithmetic
local function add(a, b) return a + b end
local function sub(a, b) return a - b end
local function mul(a, b) return a * b end

local function deep_calls()
    local r = 1
    for _ = 1, 500 do
        r = mul(add(r, 1), sub(r, 0)) - add(r, sub(1, 0))
    end
    return r
end
assert_eq(type(deep_calls()), "number", "deep nested calls produces number")

print_summary("COMPILER_KILLER_DEEP_BINOPS")
