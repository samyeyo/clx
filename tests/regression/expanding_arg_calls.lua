local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", tostring(expected), "Got:", tostring(actual))
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- expanding last-argument call emission (D1 save/restore)")

local function two()
    return 10, 20
end

local function sum3(a, b, c)
    c = c or 0
    return a + b + c
end

local obj = {
    m = function(self, a, b)
        return a * 100 + b
    end,
}

local function zero()
    return 7
end

assert_eq(sum3(1, 2, 3), 6, "fixed_args")
assert_eq(sum3(1, two()), 31, "expand_tail")
assert_eq(sum3(5, two()), 35, "expand_tail_offset")
assert_eq(sum3(two()), 30, "expand_only")
assert_eq(sum3(1, sum3(1, two())), 32, "expand_nested")
assert_eq(obj:m(2, two()), 210, "method_expand")
assert_eq(obj:m(two()), 1020, "method_expand_first")
assert_eq(zero(), 7, "zero_arg")

local s = "hello world"
local i, j = 1, 1
assert_eq(string.sub(s, i, j), "h", "sub_var_idx")
assert_eq(string.sub(s, 2, 2), "e", "sub_const_idx")

local junk = {}
for k = 1, 2000 do
    sum3(k, two())
    obj:m(k, two())
    junk[k] = "row" .. k
end
collectgarbage("collect")
assert_eq(sum3(1, two()), 31, "after_gc_pressure")
assert_eq(#junk, 2000, "junk_len")

print_summary("expanding_arg_calls")
if failed > 0 then
    error("expanding_arg_calls failed: " .. failed)
end
