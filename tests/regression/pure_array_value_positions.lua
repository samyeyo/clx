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

local function assert_true(cond, name)
    assert_eq(not not cond, true, name)
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- pure numeric array in value positions + generic-for guards")

local d = {9, 8, 7}

assert_eq(d == nil, false, "d == nil")
assert_eq(nil == d, false, "nil == d")
assert_eq(not d, false, "not d")
assert_eq(type(d), "table", "type(d)")
assert_eq(d and 1, 1, "d and 1")
assert_eq(type(d or 2), "table", "d or 2")
assert_eq((d) == nil, false, "paren (d) == nil")
assert_eq(type((d)), "table", "type((d))")

local c = 0
if d then c = c + 1 end
assert_eq(c, 1, "if condition")

local w = 0
while d and w < 2 do w = w + 1 end
assert_eq(w, 2, "while condition")

local rp = 0
repeat rp = rp + 1 until d
assert_eq(rp, 1, "repeat condition")

local cv = {d}
assert_eq(type(cv[1]), "table", "ctor bare value")
local cs = {x = d}
assert_eq(type(cs.x), "table", "ctor string-key value")
local ck = {[d] = 5}
assert_eq(ck[d], 5, "ctor computed key")
local cn = {a = {d}}
assert_eq(type(cn.a[1]), "table", "nested ctor value")

local e = d
assert_eq(e, d, "local copy identity")
e = d
assert_eq(type(e), "table", "assignment value")

GVAL = d
assert_eq(type(GVAL), "table", "global decl value")

print(d)
assert_eq(type(select(1, d)), "table", "select arg")

local function ret_paren()
    return (d)
end
assert_eq(ret_paren(), d, "return (d)")

local t2 = {}
t2[(d)] = 42
assert_eq(t2[d], 42, "t[(d)] key write")

local pd = {7}
assert_eq((pd)[1], 7, "paren table read")
;(pd)[2] = 8
assert_eq(pd[2], 8, "paren table write")

local ok, err

ok = pcall(function() local s = 0; for i = 1, d do s = s + 1 end; return s end)
assert_eq(ok, false, "for limit errors")
ok = pcall(function() local s = 0; for i = d, 2 do s = s + 1 end; return s end)
assert_eq(ok, false, "for start errors")
ok = pcall(function() local s = 0; for i = 1, 2, d do s = s + 1 end; return s end)
assert_eq(ok, false, "for step errors")
ok = pcall(function() return d + 1 end)
assert_eq(ok, false, "arith errors")
ok = pcall(function() return d .. "x" end)
assert_eq(ok, false, "concat errors")
ok = pcall(function() return -d end)
assert_eq(ok, false, "unm errors")
ok = pcall(function() return d < nil end)
assert_eq(ok, false, "compare errors")
ok = pcall(d)
assert_eq(ok, false, "call table errors")

ok = pcall(function() for x in d do end end)
assert_eq(ok, false, "table as iterator errors")
ok = pcall(function() for x in nil do end end)
assert_eq(ok, false, "nil as iterator errors")
ok = pcall(function()
    local function bad_iter()
        return {}, nil, nil
    end
    for x in bad_iter() do end
end)
assert_eq(ok, false, "iterator returning table errors")
ok = pcall(function()
    local q = {}
    for x in q do end
end)
assert_eq(ok, false, "plain table as iterator errors")

local arr = {1, 2, 3}
local k = 9
assert_eq(type(arr[k]), "nil", "OOB type() yields nil")
assert_eq(arr[k], nil, "OOB read yields nil")

local g = {0, 0, 0}
for i = 1, 10 do g[i] = i end
assert_eq(#g, 10, "# invalidates static length via loop write")

local ap = {1, 2}
ap[#ap + 1] = 3
assert_eq(#ap, 3, "# invalidates static length via append")

local nl = {1, 2}
nl[1] = nil
local border = #nl
assert_true(border == 0 or border == 2, "hole keeps a legal # border")

local lb = {1, 2}
for i = 1, 2 do lb[i] = i * 2 end
assert_eq(#lb, 2, "bounded loop write keeps static #")

local v = {9, 8, 7}
local is, vs = {}, {}
for i, x in ipairs(v) do
    is[#is + 1] = i
    vs[#vs + 1] = x
end
assert_eq(table.concat(is, ","), "1,2,3", "native ipairs indices")
assert_eq(table.concat(vs, ","), "9,8,7", "native ipairs values")

local ps = {}
for i, x in pairs(v) do
    ps[#ps + 1] = i .. "=" .. x
end
assert_eq(table.concat(ps, ","), "1=9,2=8,3=7", "native pairs sequence")

local gv = {1, 2}
local gn = 0
for i, x in ipairs(gv) do
    gn = gn + 1
    if i == 2 then gv[3] = 3 end
end
assert_eq(gn, 3, "append during iteration visible")

local iv = {5, 6}
local ivs = 0
for i, x in ipairs(iv) do ivs = ivs + x end
assert_eq(ivs, 11, "native ipairs sum")

local v2 = {1, 2, 3, 4}
local total = 0
for i, x in pairs(v2) do total = total + i * x end
assert_eq(total, 30, "native pairs weighted sum")

d = {1}
assert_eq(type(d), "table", "reassign vector local")
d = {9, 8, 7}
assert_eq(#d, 3, "restored vector length")

print_summary("REGRESSION_PURE_ARRAY_VALUE_POSITIONS")

if failed > 0 then os.exit(1) end
