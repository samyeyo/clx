local passed = 0
local failed = 0

local is_windows = (package and package.config and package.config:sub(1,1) == "\\") or os.getenv("USERPROFILE") ~= nil
local home_var = is_windows and "USERPROFILE" or "HOME"
local true_cmd = is_windows and "exit 0" or "true"

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

print("----------------- os.clock")
local c = os.clock()
assert_eq(type(c), "number", "clock returns a number")
assert_true(c >= 0, "clock is non-negative")

print("\n----------------- os.time")
local t = os.time()
assert_eq(type(t), "number", "time returns a number")
assert_true(t > 1700000000, "time is reasonable (post-2023)")

print("\n----------------- os.date")
local d1 = os.date("!%Y")
assert_eq(type(d1), "string", "date returns string")
local d2 = os.date("*t", os.time())
assert_eq(type(d2), "table", "date *t returns table")
assert_true(d2.year >= 2024, "date *t year is current")
assert_true(d2.month >= 1 and d2.month <= 12, "date *t month in range")
assert_true(d2.day >= 1 and d2.day <= 31, "date *t day in range")
assert_eq(type(d2.hour), "number", "date *t hour")
assert_eq(type(d2.min), "number", "date *t min")
assert_eq(type(d2.sec), "number", "date *t sec")
assert_eq(type(d2.wday), "number", "date *t wday")
assert_eq(type(d2.yday), "number", "date *t yday")
assert_eq(type(d2.isdst), "boolean", "date *t isdst")

print("\n----------------- os.difftime")
local t1 = os.time()
local t2 = t1 + 3600
local diff = os.difftime(t2, t1)
assert_eq(diff, 3600, "difftime 1 hour")

print("\n----------------- os.getenv")
local home = os.getenv(home_var)
assert_true(home ~= nil, "getenv " .. home_var .. " exists")
assert_eq(type(home), "string", "getenv " .. home_var .. " is string")
local no_var = os.getenv("_NONEXISTENT_VAR_XYZ_")
assert_nil(no_var, "getenv nonexistent returns nil")

print("\n----------------- os.execute")
local ok = os.execute(true_cmd)
assert_eq(type(ok), "number", "execute returns number")
local ok2 = os.execute("exit 0")
assert_eq(type(ok2), "number", "execute exit 0 returns number")
assert_eq(ok2, 0, "execute exit 0 returns 0")

print("\n----------------- os.tmpname")
local tmp = os.tmpname()
assert_eq(type(tmp), "string", "tmpname returns string")
assert_true(#tmp > 0, "tmpname non-empty")
os.remove(tmp)
assert_eq(os.getenv("_NONEXISTENT_VAR_XYZ_"), nil, "cleanup: removed tmpname still nonexistent")

print_summary("OS MODULE")