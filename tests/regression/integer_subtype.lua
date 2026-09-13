--  regression: integer subtype preservation (int+int -> int, fast functions, params, arrays)
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

local function assert_mt(actual, expected, name)
    if math.type(actual) == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected: ", expected, " Got: ", math.type(actual))
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- integer subtype preservation")

--  fast recursive function: result boxed with runtime-restored subtype
local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
assert_eq(fib(10), 55, "fast fib(10) integer")
assert_mt(fib(10), "integer", "fast fib(10) math.type")
assert_eq(fib(10.5), 99.5, "fast fib float passthrough")

--  floor division and modulo on numeric params
local function half(x) return x // 2 end
assert_eq(half(7), 3, "i//2 on param integer")
assert_mt(half(7), "integer", "i//2 on param math.type")
assert_eq(half(8.0), 4.0, "i//2 on whole-float param")

--  multi-return numeric arithmetic on params
local function g(i) return i // 2, i % 3, i * 2, i - 1, i + 10 end
local a, b, c, d, e = g(9)
assert_eq(a, 4, "multi-return i//2")
assert_eq(b, 0, "multi-return i%3")
assert_eq(c, 18, "multi-return i*2")
assert_eq(d, 8, "multi-return i-1")
assert_eq(e, 19, "multi-return i+10")

--  union-of-requirements mask: two params, result int iff both int
local function mix(x, y) return x + y end
assert_eq(mix(3, 4), 7, "mix int+int")
assert_mt(mix(3, 4), "integer", "mix int+int math.type")
assert_eq(mix(1.5, 2), 3.5, "mix float+int")
assert_mt(mix(1.5, 2), "float", "mix float+int math.type")

--  unary minus on flagged param
local function neg(x) return -x end
assert_eq(neg(5), -5, "neg int")
assert_mt(neg(5), "integer", "neg int math.type")
assert_eq(neg(5.5), -5.5, "neg float")

--  concat of flagged numeric param
local function pconca(s, n) return s .. n end
assert_eq(pconca("k", 5), "k5", "concat int param")
assert_eq(pconca("k", 2.5), "k2.5", "concat float param")

--  numeric param reassigned to float: flag must clear (stale-flag bug)
local function rp(x)
    x = x * 1.0
    return x
end
assert_eq(rp(5), 5.0, "param reassigned float value")
assert_mt(rp(5), "float", "param reassigned float math.type")

--  numeric param reassigned to int expression: flag must set
local function ri(x)
    x = x + 1
    return x, math.type(x)
end
local v1, v2 = ri(6)
assert_eq(v1, 7, "param reassigned int value")
assert_eq(v2, "integer", "param reassigned int math.type")

--  local demoted by float reassignment keeps subtype per value (probe6)
local function dd(flag)
    local d = 7
    local r1 = d + 1
    d = 2.5
    local r2 = d + 1
    return r1, r2, math.type(d)
end
local r1, r2, mt = dd(true)
assert_eq(r1, 8, "pre-demotion read integer")
assert_mt(r1, "integer", "pre-demotion read math.type")
assert_eq(r2, 3.5, "post-demotion read float")
assert_mt(r2, "float", "post-demotion read math.type")
assert_eq(mt, "float", "demoted local math.type")

--  same-name shadow decl of a param evaluates RHS against outer binding
local function sh(i)
    local i = i + 1
    return i
end
assert_eq(sh(2), 3, "shadow decl RHS uses outer binding")
assert_mt(sh(2), "integer", "shadow decl result integer")
assert_eq(sh(2.5), 3.5, "shadow decl float passthrough")

--  same-block redecl of int64-typed local (probe5)
local x = 1.5
local x = 5
assert_eq(x, 5, "same-block redecl value")
assert_mt(x, "integer", "same-block redecl math.type")

--  int64-typed local with later float reassignment (redecl probe)
local y = 5
y = 2.5
assert_eq(y, 2.5, "int64 local demoted value")
assert_mt(y, "float", "int64 local demoted math.type")

--  integer-typed promoted array: constructor + loop writes
local t = {10, 20, 30}
for i = 4, 6 do t[i] = i * 10 end
assert_eq(t[1], 10, "int array read 1")
assert_eq(t[6], 60, "int array read 6")
assert_mt(t[4], "integer", "int array math.type")
local sum = 0
for _, v in ipairs(t) do sum = sum + v end
assert_eq(sum, 210, "int array sum")

--  nested fast calls preserve subtype through wrappers
local function idn(x) return x end
local function addone(x) return idn(x) + 1 end
assert_eq(addone(41), 42, "nested fast call integer")
assert_mt(addone(41), "integer", "nested fast call math.type")
assert_eq(addone(41.5), 42.5, "nested fast call float")

--  runtime int ops via boxed LValues (math.maxinteger passthrough).
--  int64 overflow wraps like stock Lua (two's-complement), staying integer.
assert_eq(math.maxinteger + 1, math.mininteger, "maxint+1 wraps to mininteger (Lua semantics)")
assert_eq(math.mininteger - 1, math.maxinteger, "minint-1 wraps to maxinteger (Lua semantics)")
assert_eq(-math.mininteger, math.mininteger, "unm mininteger wraps (Lua semantics)")
assert_eq(math.mininteger // -1, math.mininteger, "minint//-1 wraps (Lua semantics)")
assert_eq(math.type(math.maxinteger + 1), "integer", "wrap result stays integer")

print_summary("integer subtype")
if failed > 0 then error("integer subtype regression failed") end
