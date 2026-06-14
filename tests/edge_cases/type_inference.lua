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

print("----------------- Complex type inference patterns")

-- Table that changes value type over time (confuses purity)
local function morphing_table()
    local t = {}
    t[1] = 10
    t[2] = "hello"
    t[3] = { x = 1 }
    assert_eq(t[1], 10, "morphing [1] number")
    assert_eq(t[2], "hello", "morphing [2] string")
    assert_eq(t[3].x, 1, "morphing [3] table")
end
morphing_table()

-- Conditional type assignment
local function conditional_type(flag)
    local t = {}
    if flag then
        t[1] = 42
    else
        t[1] = "fallback"
    end
    return t[1]
end
assert_eq(conditional_type(true), 42, "conditional type true -> number")
assert_eq(conditional_type(false), "fallback", "conditional type false -> string")

-- Upvalue type confusion
local function type_confusion()
    local v = 0
    local function setter(x)
        v = x
    end
    local function getter()
        return v
    end
    setter(42)
    assert_eq(getter(), 42, "upvalue set/get number")
    setter("str")
    assert_eq(getter(), "str", "upvalue set/get string")
    setter({ key = "val" })
    assert_eq(getter().key, "val", "upvalue set/get table")
end
type_confusion()

-- Array of mixed types
local function mixed_type_array()
    local arr = {}
    arr[1] = true
    arr[2] = 3.14
    arr[3] = "text"
    arr[4] = nil
    arr[5] = { nested = true }
    assert_true(arr[1], "mixed array [1] bool")
    assert_eq(arr[2], 3.14, "mixed array [2] float")
    assert_eq(arr[3], "text", "mixed array [3] string")
    assert_true(arr[5].nested, "mixed array [5] table")
end
mixed_type_array()

-- Function returning different types
local function type_variant(sel)
    if sel == 1 then return 42 end
    if sel == 2 then return "hello" end
    if sel == 3 then return {} end
    return nil
end
assert_eq(type_variant(1), 42, "type variant 1 number")
assert_eq(type_variant(2), "hello", "type variant 2 string")
assert_eq(type(type_variant(3)), "table", "type variant 3 table")
assert_eq(type_variant(4), nil, "type variant 4 nil")

print_summary("COMPILER_KILLER_TYPE_INFERENCE")
