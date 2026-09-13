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

print("----------------- io.open failure returns nil, errmsg (Lua semantics)")

-- Regression: io.open on a missing file threw an error instead of returning
-- nil plus an error message. Standard Lua returns nil, errmsg; tlcli's module
-- resolution depends on this.
local f, err = io.open("/tmp/_clx_nonexistent_regression_file_", "r")
assert_true(f == nil, "open non-existent returns nil")
assert_true(err ~= nil, "open non-existent returns error message")
assert_true(type(err) == "string", "error message is a string")

-- valid open still works
local path = os.tmpname()
local w = io.open(path, "w")
assert_true(w ~= nil, "open for write succeeds")
w:write("hello")
w:close()
local r = io.open(path, "r")
assert_true(r ~= nil, "open for read succeeds")
assert_eq(r:read("a"), "hello", "read back content")
r:close()
os.remove(path)

print("----------------- integer arithmetic keeps integer typing")

-- Regression: Int64 fast paths promoted %, //, unary - results to double,
-- producing keys like "opt_1.0" instead of "opt_1".
local keys = {}
for i = 1, 4 do
    local k = "opt_" .. tostring(i % 2)
    keys[k] = (keys[k] or 0) + 1
end
assert_eq(keys["opt_0"], 2, "opt_0 count (integer key)")
assert_eq(keys["opt_1"], 2, "opt_1 count (integer key)")
assert_eq(keys["opt_0.0"], nil, "no float-form key created")
assert_eq(keys["opt_1.0"], nil, "no float-form key created")

assert_eq(7 % 2, 1, "int mod stays integer")
assert_eq(-7 // 2, -4, "int idiv floors")
assert_eq(-(2 ^ 3), -8.0, "unm on power promotes to float")

print_summary("IO_AND_INT_SEMANTICS")
if failed > 0 then
    error("io_and_int_semantics: " .. failed .. " failures")
end
