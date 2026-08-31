local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected: ", expected, " Got: ", actual)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- captured variable + string builder")

local function concat_inside_closure()
    local s = ""
    local add = function(x) s = s .. x end
    for i = 1, 3 do add(tostring(i)) end
    return s
end
assert_eq(concat_inside_closure(), "123", "concat inside nested closure")

local function concat_declaring_scope()
    local s = ""
    local function get() return s end
    s = s .. "!"
    return get()
end
assert_eq(concat_declaring_scope(), "!", "nested read sees declaring-scope concat")

local function nested_write_outer_read()
    local s = "a"
    local function set() s = s .. "b" end
    set()
    return s
end
assert_eq(nested_write_outer_read(), "ab", "nested write visible to declaring scope")

local function captured_param(s)
    local function add(x) s = s .. x end
    add("!")
    return s
end
assert_eq(captured_param("hello"), "hello!", "captured parameter concat")

local function captured_loop_var()
    local t = { "a", "b", "c" }
    local out = ""
    for k, v in ipairs(t) do
        local g = function() return v end
        v = v .. "x"
        out = out .. g()
    end
    return out
end
assert_eq(captured_loop_var(), "axbxcx", "captured loop variable concat")

local function plain_concat()
    local s = ""
    for i = 1, 3 do s = s .. tostring(i) end
    return s
end
assert_eq(plain_concat(), "123", "non-captured builder still correct")

print_summary("CAPTURED_STRING_BUILDER")

if failed > 0 then
    os.exit(1)
end
