local failures = 0
local function assert_eq(actual, expected, name)
    if actual == expected then
        print("[OK] " .. name)
    else
        failures = failures + 1
        print("[FAIL] " .. name .. " actual=" .. tostring(actual) .. " expected=" .. tostring(expected))
    end
end

local mt = { __eq = function(a, b) return "eq-called" end }
local t1, t2 = setmetatable({}, mt), setmetatable({}, mt)
local mto = { __eq = function() return "other" end }
local t3, t4 = setmetatable({}, mto), setmetatable({}, mto)

assert_eq(t1 == t2, true, "eq-mt-true-branch")
local r1 = rawequal(t1, t2)
assert_eq(r1, false, "rawequal-diff-ptr")

local called = false
local mt2 = { __eq = function(a, b) called = true return a.x == b.x end }
local a, b = setmetatable({ x = 1 }, mt2), setmetatable({ x = 1 }, mt2)
assert_eq(a == b, true, "eq-mt-method-result")
assert_eq(called, true, "eq-mt-method-called")

assert_eq(t1 == nil, false, "table-vs-nil-false")
assert_eq(nil == t1, false, "nil-vs-table-false")
assert_eq(t1 == 5, false, "table-vs-number-false")
assert_eq(t1 == "s", false, "table-vs-string-false")
assert_eq(false == nil, false, "bool-vs-nil")
assert_eq("x" == 5, false, "string-vs-number")
assert_eq(t3 == t4, true, "other-mt-eq")
assert_eq(t3 == t1, false, "cross-mt-false")
assert_eq(t1 ~= nil, true, "ne-table-nil")

local n, f = 1, 1.0
assert_eq(n == f, true, "int-float-eq")
assert_eq(n == 2, false, "int-diff")
assert_eq("abc" == "abc", true, "str-eq")
assert_eq("abc" == "abd", false, "str-ne")

local ok, err = pcall(function() return 1 + {} end)
assert_eq(ok, false, "add-type-error")
assert_eq(type(err) == "string" and err:find("attempt to perform arithmetic on a table value") ~= nil, true, "add-err-msg")
assert_eq(type(err) == "string" and err:find(":%d+:") ~= nil, true, "add-err-has-lineno")

local ok2, err2 = pcall(function() return {} < {} end)
assert_eq(ok2, false, "lt-type-error")
assert_eq(type(err2) == "string" and err2:find("attempt to compare") ~= nil, true, "lt-err-msg")

local ok3, err3 = pcall(function() return "a" .. {} end)
assert_eq(ok3, false, "concat-type-error")
assert_eq(type(err3) == "string" and err3:find("attempt to concatenate") ~= nil, true, "concat-err-msg")

local ok4, err4 = pcall(function() return {} < 1 end)
local ok5, err5 = pcall(function() return 1 < {} end)
assert_eq(ok5, false, "cross-lt2-error")
assert_eq(type(err5) == "string" and err5:find("attempt to compare number with table") ~= nil, true, "cross-lt2-msg")
assert_eq(type(err4) == "string" and err4:find("attempt to compare table with number") ~= nil, true, "cross-lt-msg-exact")
local ok6, err6 = pcall(function() return true & 1 end)
assert_eq(type(err6) == "string" and err6:find("attempt to perform bitwise operation on a boolean value") ~= nil, true, "bitwise-msg")
local ok7, err7 = pcall(function() return #true end)
assert_eq(type(err7) == "string" and err7:find("attempt to get length of a boolean value") ~= nil, true, "len-msg")
assert_eq(ok4, false, "cross-lt-error")
assert_eq(type(err4) == "string" and err4:find("attempt to compare") ~= nil, true, "cross-lt-msg")

if failures > 0 then
    print(failures .. " test(s) FAILED")
else
    print("All tests passed")
end
