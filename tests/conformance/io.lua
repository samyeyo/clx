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

print("----------------- io.type")
assert_nil(io.type(42), "type number -> nil")
assert_nil(io.type("foo"), "type string -> nil")
assert_nil(io.type({}), "type table -> nil")

print("\n----------------- io.tmpfile")
local tmp = io.tmpfile()
assert_eq(io.type(tmp), "file", "tmpfile type is file")
tmp:close()

print("\n----------------- io.flush")
print("BEFORE flush")
io.flush()
print("AFTER flush")
assert_true(true, "flush does not crash")

print("\n----------------- tmpfile:write / seek / read")
local f1 = io.tmpfile()
f1:write("hello world")
f1:seek("set", 0)
local content = f1:read("*a")
assert_eq(content, "hello world", "tmpfile write+read back")
f1:close()

print("\n----------------- tmpfile:multi-write / seek / read")
local f2 = io.tmpfile()
f2:write("abc", "def", "ghi")
f2:seek("set", 0)
assert_eq(f2:read("*a"), "abcdefghi", "tmpfile multi-write")
f2:close()

print("\n----------------- tmpfile:seek")
local f3 = io.tmpfile()
f3:write("1234567890")
assert_eq(f3:seek("end"), 10, "seek end returns size")
f3:seek("set", 3)
assert_eq(f3:read(3), "456", "read after seek set")
f3:close()

print("\n----------------- io.open / close (non-existent)")
local ok, err = pcall(io.open, "/tmp/_clx_nonexistent_test_file_", "r")
assert_true(not ok, "open non-existent raises error")

print("\n----------------- io.input / output default")
local def_in = io.input()
assert_eq(io.type(def_in), "file", "default input is file")
local def_out = io.output()
assert_eq(io.type(def_out), "file", "default output is file")

print("\n----------------- tmpfile:close then type")
local f4 = io.tmpfile()
f4:write("data")
f4:close()
assert_eq(io.type(f4), "closed file", "type after close")

print("\n----------------- tmpfile:lines")
local f5 = io.tmpfile()
f5:write("line1\nline2\nline3")
f5:seek("set", 0)
local lines = {}
for l in f5:lines() do
    lines[#lines + 1] = l
end
assert_eq(#lines, 3, "lines count")
assert_eq(lines[1], "line1", "lines[1]")
assert_eq(lines[2], "line2", "lines[2]")
assert_eq(lines[3], "line3", "lines[3]")
f5:close()

print("\n----------------- tmpfile:flush")
local f6 = io.tmpfile()
f6:write("flush test")
f6:flush()
f6:seek("set", 0)
assert_eq(f6:read("*a"), "flush test", "flush then read")
f6:close()

print_summary("IO MODULE")
