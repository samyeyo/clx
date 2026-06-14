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

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- Tables")

local t1 = { x = 10, y = 20 }
assert_eq(t1.x, 10, "dictionary literal x")
assert_eq(t1.y, 20, "dictionary literal y")

local t2 = {}
t2.nested = { z = 99 }
assert_eq(t2.nested.z, 99, "nested table access")

local t3 = { 50, 60, 70 }
assert_eq(t3[1], 50, "array constructor index 1")
assert_eq(t3[3], 70, "array constructor index 3")

print("\n----------------- Vararg Table Constructor {...}")

local function pack(...)
    local t = {...}
    return t
end
local p1 = pack(1, 2, 3, 4)
assert_eq(#p1, 4, "{...} count")
assert_eq(p1[1], 1, "{...} index 1")
assert_eq(p1[4], 4, "{...} index 4")

local p2 = pack()
assert_eq(#p2, 0, "empty {...} count")

local function mixed(a, b, ...)
    local t = {...}
    return a, b, t
end
local ma, mb, mt = mixed(10, 20, 30, 40)
assert_eq(ma, 10, "mixed: fixed param 1")
assert_eq(mb, 20, "mixed: fixed param 2")
assert_eq(#mt, 2, "mixed: vararg count")
assert_eq(mt[1], 30, "mixed: vararg 1")
assert_eq(mt[2], 40, "mixed: vararg 2")

print("\n----------------- Object-Oriented Colon Syntax (:)")

local obj = { value = 100 }

-- Define using colon syntax (implicit self)
function obj:add(amount)
    self.value = self.value + amount
    return self.value
end

-- Call using colon syntax
local res1 = obj:add(50)
assert_eq(obj.value, 150, "implicit self method call (mutation)")
assert_eq(res1, 150, "implicit self method call (return)")

-- Call using dot notation to prove equivalence
local res2 = obj.add(obj, 20)
assert_eq(obj.value, 170, "method call via dot notation (mutation)")
assert_eq(res2, 170, "method call via dot notation (return)")

print("\n----------------- table.concat")
local cat1 = table.concat({10, 20, 30}, ",")
assert_eq(cat1, "10,20,30", "concat with sep")
local cat2 = table.concat({"a", "b", "c"})
assert_eq(cat2, "abc", "concat no sep")
local cat3 = table.concat({}, ",")
assert_eq(cat3, "", "concat empty")
local cat4 = table.concat({"x", "y", "z"}, "-", 2, 3)
assert_eq(cat4, "y-z", "concat with i,j")

print("\n----------------- table.insert")
local ins = {10, 20, 40}
table.insert(ins, 3, 30)
assert_eq(ins[1], 10, "insert pos: [1]")
assert_eq(ins[2], 20, "insert pos: [2]")
assert_eq(ins[3], 30, "insert pos: [3]")
assert_eq(ins[4], 40, "insert pos: [4]")
table.insert(ins, 50)
assert_eq(ins[5], 50, "insert end: [5]")
assert_eq(#ins, 5, "insert end: len")

print("\n----------------- table.remove")
local rem = {10, 20, 30, 40}
local rv = table.remove(rem, 2)
assert_eq(rv, 20, "remove returns value")
assert_eq(rem[1], 10, "remove shifts: [1]")
assert_eq(rem[2], 30, "remove shifts: [2]")
assert_eq(rem[3], 40, "remove shifts: [3]")
assert_eq(#rem, 3, "remove len")
local rv2 = table.remove(rem)
assert_eq(rv2, 40, "remove last: value")
assert_eq(#rem, 2, "remove last: len")
local nil_rem = table.remove({}, 1)
assert_eq(nil_rem, nil, "remove empty returns nil")

print("\n----------------- table.sort")
local srt = {3, 1, 4, 1, 5, 9}
table.sort(srt)
assert_eq(srt[1], 1, "sort asc [1]")
assert_eq(srt[2], 1, "sort asc [2]")
assert_eq(srt[3], 3, "sort asc [3]")
assert_eq(srt[4], 4, "sort asc [4]")
assert_eq(srt[5], 5, "sort asc [5]")
assert_eq(srt[6], 9, "sort asc [6]")
local srt2 = {"a", "ccc", "bb"}
table.sort(srt2, function(a, b) return #a < #b end)
assert_eq(srt2[1], "a", "sort by length [1]")
assert_eq(srt2[2], "bb", "sort by length [2]")
assert_eq(srt2[3], "ccc", "sort by length [3]")
table.sort(srt2, function(a, b) return a > b end)
assert_eq(srt2[1], "ccc", "sort desc [1]")
assert_eq(srt2[3], "a", "sort desc [3]")

print("\n----------------- table.pack")
local pk = table.pack(10, 20, 30)
assert_eq(pk[1], 10, "pack [1]")
assert_eq(pk[2], 20, "pack [2]")
assert_eq(pk[3], 30, "pack [3]")
assert_eq(pk.n, 3, "pack .n")
local pk2 = table.pack()
assert_eq(pk2.n, 0, "pack empty .n")

print("\n----------------- table.unpack")
local up = {10, 20, 30}
local a1, a2, a3 = table.unpack(up)
assert_eq(a1, 10, "unpack [1]")
assert_eq(a2, 20, "unpack [2]")
assert_eq(a3, 30, "unpack [3]")
local a, b = table.unpack(up, 2, 3)
assert_eq(a, 20, "unpack range [2]")
assert_eq(b, 30, "unpack range [3]")
local u1, u2 = table.unpack({100}, 1, 1)
assert_eq(u1, 100, "unpack single")
assert_eq(u2, nil, "unpack single second nil")

print("\n----------------- table.move")
local mv_src = {1, 2, 3, 4, 5}
local mv_dst = {10, 20, 30, 40, 50}
table.move(mv_src, 1, 3, 2, mv_dst)
assert_eq(mv_dst[1], 10, "move: dst[1] unchanged")
assert_eq(mv_dst[2], 1, "move: dst[2]")
assert_eq(mv_dst[3], 2, "move: dst[3]")
assert_eq(mv_dst[4], 3, "move: dst[4]")
assert_eq(mv_dst[5], 50, "move: dst[5] unchanged")
local mv2 = {1, 2, 3, 4, 5}
table.move(mv2, 3, 5, 1)
assert_eq(mv2[1], 3, "move within same: src[1]")
assert_eq(mv2[2], 4, "move within same: src[2]")
assert_eq(mv2[3], 5, "move within same: src[3]")
assert_eq(mv2[4], 4, "move within same: src[4]")
assert_eq(mv2[5], 5, "move within same: src[5]")

print_summary("TABLES")