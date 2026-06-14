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

local function assert_nil(val, name)
    if val == nil then
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

print("----------------- Goto crossing local declarations")

-- Goto jumps past a local with simple initializer (constant)
local function test_goto_cross()
    local x = 1
    goto skip
    ::skip::
    local y = 5
    return x + y
end
assert_eq(test_goto_cross(), 6, "goto crosses local with constant initializer")

-- Goto in nested block
local function test_goto_nested()
    local r = 0
    do
        goto inner
        local z = 99
        ::inner::
        r = 1
    end
    return r
end
assert_eq(test_goto_nested(), 1, "goto crosses local in nested block")

-- Goto inside loop
local function test_goto_in_loop()
    local sum = 0
    for i = 1, 5 do
        if i == 3 then goto next end
        sum = sum + i
        ::next::
    end
    return sum
end
assert_eq(test_goto_in_loop(), 1 + 2 + 4 + 5, "goto inside for loop skips iteration body")

-- Goto with label before a block
local function test_goto_before_block()
    local acc = 0
    goto lab
    ::lab::
    do
        local x = 42
        acc = acc + x
    end
    return acc
end
assert_eq(test_goto_before_block(), 42, "goto to label before block")

print_summary("REGRESSION_GOTO_HOIST")
