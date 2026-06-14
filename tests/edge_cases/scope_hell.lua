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

local function assert_true(val, name)
    if val then
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

print("----------------- Deeply nested blocks and scoping")

-- 10 levels of do..end
local function deep_blocks()
    local x = 0
    do local a = 1; x = x + a
    do local b = 2; x = x + b
    do local c = 3; x = x + c
    do local d = 4; x = x + d
    do local e = 5; x = x + e
    do local f = 6; x = x + f
    do local g = 7; x = x + g
    do local h = 8; x = x + h
    do local i = 9; x = x + i
    do local j = 10; x = x + j
    end end end end end end end end end end
    return x
end
assert_eq(deep_blocks(), 55, "10 deep nested blocks sum 1..10")

-- Deeply shadowed locals
local function deep_shadow()
    local v = 0
    do local v = 10
    do local v = 20
    do local v = 30
    do local v = 40
    do local v = 50
        v = v + 5
        assert_eq(v, 55, "deep shadow innermost v")
    end end end end end
    return v
end
assert_eq(deep_shadow(), 0, "deep shadow outermost restored to 0")

-- Scopes with upvalues at many depths
local function upvalue_chain()
    local a, b, c, d, e = 1, 2, 3, 4, 5
    local function f1()
        local function f2()
            local function f3()
                return a + b + c + d + e
            end
            return f3()
        end
        return f2()
    end
    return f1()
end
assert_eq(upvalue_chain(), 15, "3-deep closure chain captures 5 upvalues")

print_summary("COMPILER_KILLER_SCOPE_HELL")
