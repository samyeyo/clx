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

print("----------------- StringBuilder edge cases")

-- Module-level .. chain (StringBuilder detection)
local function concat_chain(a, b, c)
    return a .. b .. c
end
assert_eq(concat_chain("x", "y", "z"), "xyz", "simple concat chain")

-- Long concat chain
local function long_concat(n)
    local s = ""
    for i = 1, n do
        s = s .. tostring(i)
    end
    return #s
end
assert_eq(long_concat(100), 192, "long concat chain length 100")
assert_eq(long_concat(500), 1392, "long concat chain length 500")

-- Concat with nil guard
local function concat_nil_guard(a, b)
    local sa = a or ""
    local sb = b or ""
    return sa .. sb
end
assert_eq(concat_nil_guard("hello", "world"), "helloworld", "concat with nil guard both non-nil")
assert_eq(concat_nil_guard(nil, "world"), "world", "concat with nil guard first nil")
assert_eq(concat_nil_guard("hello", nil), "hello", "concat with nil guard second nil")

-- Alternating string/number concat
local function mixed_concat()
    return "value: " .. 42 .. " (" .. 3.14 .. ")"
end
assert_eq(mixed_concat(), "value: 42 (3.14)", "mixed string/number concat")

-- Concat in a table context
local function table_concat_keys()
    local t = {}
    for i = 1, 10 do
        t["idx_" .. i] = i
    end
    local sum = 0
    for i = 1, 10 do
        sum = sum + t["idx_" .. i]
    end
    return sum
end
assert_eq(table_concat_keys(), 55, "table with concat keys sum")

-- Module-level StringBuilder (if compiler detects it)
local function module_string_build()
    local parts = {}
    for i = 1, 1000 do
        parts[i] = tostring(i)
    end
    return table.concat(parts, ",")
end
local csv = module_string_build()
local count = 0
for _ in csv:gmatch(",") do count = count + 1 end
assert_eq(count, 999, "module_string_build has 999 commas")

print_summary("COMPILER_KILLER_STRINGBUILDER")
