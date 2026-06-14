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

local function assert_vec_eq(vec, exp_x, exp_y, name)
    if vec.x == exp_x then
        if vec.y == exp_y then
            passed = passed + 1
            print("[OK]   ", name)
        else
            failed = failed + 1
            print("[FAIL] ", name)
        end
    else
        failed = failed + 1
        print("[FAIL] ", name)
    end
end

local function print_summary()
    print("-----")
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- Metatable test")

local VectorMeta = {}
VectorMeta.__index = VectorMeta

VectorMeta.__add = function(a, b)
    local res = {x = a.x + b.x, y = a.y + b.y}
    setmetatable(res, VectorMeta)
    return res
end

VectorMeta.__sub = function(a, b)
    local res = {x = a.x - b.x, y = a.y - b.y}
    setmetatable(res, VectorMeta)
    return res
end

VectorMeta.__mul = function(a, b)
    local res = {x = a.x * b.x, y = a.y * b.y}
    setmetatable(res, VectorMeta)
    return res
end

VectorMeta.__div = function(a, b)
    local res = {x = a.x / b.x, y = a.y / b.y}
    setmetatable(res, VectorMeta)
    return res
end

VectorMeta.__eq = function(a, b)
    return a.x == b.x and a.y == b.y
end

VectorMeta.__lt = function(a, b)
    return (a.x * a.x + a.y * a.y) < (b.x * b.x + b.y * b.y)
end

VectorMeta.__le = function(a, b)
    return (a.x * a.x + a.y * a.y) <= (b.x * b.x + b.y * b.y)
end

local function create_vector(x, y)
    local v = {x = x, y = y}
    setmetatable(v, VectorMeta)
    return v
end

local v1 = create_vector(10, 20)
local v2 = create_vector(2, 4)
local v3 = create_vector(10, 20)

assert_vec_eq(v1, 10, 20, "vector creation")

local v_add = v1 + v2
assert_vec_eq(v_add, 12, 24, "__add")

local v_sub = v1 - v2
assert_vec_eq(v_sub, 8, 16, "__sub")

local v_mul = v1 * v2
assert_vec_eq(v_mul, 20, 80, "__mul")

local v_div = v1 / v2
assert_vec_eq(v_div, 5, 5, "__div")

assert_eq(v1 == v3, true, "__eq (==)")
assert_eq(v1 == v2, false, "__eq (== false)")
assert_eq(v1 ~= v2, true, "__eq (~=)")

assert_eq(v2 < v1, true, "__lt (<)")
assert_eq(v1 > v2, true, "__lt (>)")

assert_eq(v2 <= v1, true, "__le (<=)")
assert_eq(v1 <= v3, true, "__le (<= equal)")
assert_eq(v1 >= v2, true, "__le (>=)")

local Animal = { legs = 4 }
local AnimalMeta = { __index = Animal }
local dog = {}
setmetatable(dog, AnimalMeta)

assert_eq(dog.legs, 4, "__index")

local locked_table = {}
local LockMeta = { 
    __newindex = function(t, k, v) 
    end 
}
setmetatable(locked_table, LockMeta)
locked_table.prop = 99

assert_eq(locked_table.prop, nil, "__newindex")

local CallableTable = { base = 100 }
local CallMeta = {
    __call = function(t, a, b)
        return t.base + a + b
    end
}
setmetatable(CallableTable, CallMeta)

local call_res = CallableTable(10, 5)
assert_eq(call_res, 115, "__call")

local StringWrapper = { str = "clx" }
local StringMeta = {
    __len = function(t)
        return 999
    end,
    __concat = function(a, b)
        local a_val = type(a) == "table" and a.str or a
        local b_val = type(b) == "table" and b.str or b
        return a_val .. "-" .. b_val
    end
}
setmetatable(StringWrapper, StringMeta)

assert_eq(#StringWrapper, 999, "__len")
assert_eq(StringWrapper .. "rocks", "clx-rocks", "__concat")

local raw_table = {10, 20, 30}
assert_eq(#raw_table, 3, "primitive #table")
assert_eq("test" .. 123, "test123", "primitive concat")

local NamedObject = {}
setmetatable(NamedObject, { __name = "MyCustomType" })

local str_output = tostring(NamedObject)
assert_eq(string.sub(str_output, 1, 12), "MyCustomType", "__name prefix changes output")

local MathMeta = {
    __mod = function(a, b) return (a.base % b.base) * 10 end,
    __idiv = function(a, b) return (a.base // b.base) * 10 end,
    __pow = function(a, b) return (a.base ^ b.base) * 10 end,
    __unm = function(a) return -a.base * 10 end
}

local m1 = { base = 10 }
local m2 = { base = 3 }
setmetatable(m1, MathMeta)
setmetatable(m2, MathMeta)

assert_eq(m1 % m2, 10, "__mod")
assert_eq(m1 // m2, 30, "__idiv")
assert_eq(m1 ^ m2, 10000, "__pow")
assert_eq(-m1, -100, "__unm")

local protected_table = {}
setmetatable(protected_table, {
    __metatable = "Access Denied"
})

assert_eq(getmetatable(protected_table), "Access Denied", "getmetatable respects __metatable")

local is_destroyed = false

do
    local t = {}
    setmetatable(t, {
        __gc = function(obj)
            is_destroyed = true
        end
    })
end

collectgarbage("collect")
assert_eq(is_destroyed, true, "__gc metamethod fired")

local BitMeta = {
    __band = function(a, b) return a.val & b.val end,
    __bnot = function(a) return ~(a.val) end
}
local flags = setmetatable({val = 12}, BitMeta)
local mask = setmetatable({val = 4}, BitMeta)

assert_eq(flags & mask, 4, "__band metamethod")
assert_eq(~mask, -5, "__bnot metamethod")

print("\n----------------- Generic For & Iterators")

local kv_table = { a = 1, b = 2, c = 3 }
local count = 0
for k, v in pairs(kv_table) do
    count = count + 1
end
assert_eq(count, 3, "generic for with pairs()")

-- Test __pairs metamethod
local meta_obj = { hidden = "secret" }
setmetatable(meta_obj, {
    __pairs = function(t)
        local function iter(tbl, k)
            if k == nil then return "key", tbl.hidden end
            return nil
        end
        return iter, t, nil
    end
})

local meta_found = false
for k, v in pairs(meta_obj) do
    if k == "key" and v == "secret" then meta_found = true end
end
assert_eq(meta_found, true, "__pairs metamethod support")

print("\n----------------- Integer-Indexed Table with __index")

local int_defaults = { __index = function(t, k) return k * 100 end }
local int_table = { 10, 20, 30 }
setmetatable(int_table, int_defaults)

assert_eq(int_table[1], 10, "int-indexed table: existing key [1]")
assert_eq(int_table[2], 20, "int-indexed table: existing key [2]")
assert_eq(int_table[3], 30, "int-indexed table: existing key [3]")
assert_eq(int_table[4], 400, "int-indexed table: __index fallback [4]")
assert_eq(int_table[10], 1000, "int-indexed table: __index fallback [10]")

local side_effect_count = 0
local int_meta2 = { __index = function(t, k) side_effect_count = side_effect_count + 1; return -1 end }
local int_table2 = { 1, 2, 3 }
setmetatable(int_table2, int_meta2)

local v1 = int_table2[1]
assert_eq(v1, 1, "int-indexed: no __index on existing key")
assert_eq(side_effect_count, 0, "int-indexed: __index not called for existing key")

local v5 = int_table2[5]
assert_eq(v5, -1, "int-indexed: __index called for missing key")
assert_eq(side_effect_count, 1, "int-indexed: __index called exactly once")

local int_table3 = { 100, 200 }
local int_meta3 = { __index = { [1] = 999, [2] = 888, [3] = 777 } }
setmetatable(int_table3, int_meta3)

assert_eq(int_table3[1], 100, "int-indexed: own value overrides __index table [1]")
assert_eq(int_table3[2], 200, "int-indexed: own value overrides __index table [2]")
assert_eq(int_table3[3], 777, "int-indexed: __index table provides fallback [3]")

print_summary()