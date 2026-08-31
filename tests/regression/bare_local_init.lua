local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", tostring(actual))
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- bare local without initializer assigned non-numeric")

local fn
fn = function() return 42 end
assert_eq(fn(), 42, "bare local fn assigned closure")

local t
t = { x = 1, y = 2 }
assert_eq(t.x + t.y, 3, "bare local t assigned table")

local s
s = "hello"
assert_eq(s .. " world", "hello world", "bare local s assigned string")

local fn2
fn2 = function() return 99 end
assert_eq(fn2(), 99, "bare local fn2 closure second assign")

local s2
s2 = "foo"
s2 = s2 .. "bar"
assert_eq(s2, "foobar", "bare local s2 concat after assign")

-- control: ensure nil/false init still works (should not regress)
local fn_nil = nil
fn_nil = function() return 42 end
assert_eq(fn_nil(), 42, "local fn=nil assigned closure still works")

local fn_false = false
fn_false = function() return 42 end
assert_eq(fn_false(), 42, "local fn=false assigned closure still works")

print_summary("REGRESSION_BARE_LOCAL_INIT")

if failed > 0 then os.exit(1) end
