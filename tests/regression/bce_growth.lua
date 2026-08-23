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

local function assert_str_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", actual)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- BCE: empty table filled in loop (no raw array pokes)")

-- Bug: bounds-check-elimination fast paths emitted raw LTable.array[] stores/loads
-- without capacity checks. Tables created empty ({}) have array_cap == 0 and a null
-- array pointer -> SIGSEGV on first X[i] = ... store.

local N = 30
local X = {}
local Y = {}

for i = 1, N do
    X[i] = i * 2
    Y[i] = i * 3
end

assert_eq(#X, 30, "empty table grown via BCE stores: length")

local sum = 0
for i = 2, N do
    local x = (1 - 0.5) * X[i - 1] + 0.5 * X[i]
    local y = (1 - 0.5) * Y[i - 1] + 0.5 * Y[i]
    sum = sum + x + y
end
-- x+y per iteration = (2i-1) + (3i-1.5); sum over i=2..30 = 2247.5
assert_eq(sum, 2247.5, "BCE loads after growth: interpolation sum")
assert_eq(X[1], 2, "BCE load [1]")
assert_eq(X[30], 60, "BCE load [N]")

-- mixed keys: hash-part fallback must still work through the same path
X["k"] = "str"
assert_str_eq(X.k, "str", "string key coexists with numeric BCE stores")

print("----------------- Long strings: leading newline stripped per Lua 5.5 3.1")

local a = [[
abc]]
assert_eq(#a, 3, "[[ plus newline: newline stripped")

local b = [==[
x]==]
assert_eq(#b, 1, "[==[ plus newline: newline stripped")

local c = [[no leading newline]]
assert_eq(#c, 18, "no leading newline: content unchanged")

local d = [[

two]]
assert_eq(#d, 4, "only the FIRST newline is stripped")

local e = [=[=x]=]
assert_eq(e, "=x", "leveled string without newline unchanged")

print_summary("REGRESSION_BCE_GROWTH_AND_LONGSTR")
