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

local u = require("capi_lightud_mod")
local codes = u.codes()

-- 1) type identity: light userdata is its own type, distinct from full userdata
local a = u.mkptr(1)
assert_eq(u.type(a), codes.lightuserdata, "lightuserdata has LUA_TLIGHTUSERDATA")
assert_eq(u.type({}), codes.table, "table type is unchanged")
assert_eq(u.isuserdata(a), true, "lua_isuserdata accepts light userdata")
assert_eq(u.ptrval(a), 1, "lua_touserdata returns the pointer")
assert_eq(u.toptr(a), 1, "lua_topointer returns the pointer")

-- 2) equality compares addresses, it never dereferences the pointer
local b = u.mkptr(2)
assert_eq(a == a, true, "same pointer is equal")
assert_eq(a == b, false, "different pointers differ")
assert_eq(a == u.mkptr(1), true, "a fresh value with the same address is equal")
assert_eq(a == {}, false, "light userdata never equals a table")
assert_eq(a == nil, false, "light userdata is not nil")
local okord, errord = pcall(function() return a < b end)
assert_eq(okord, false, "light userdata is not ordered")
assert_eq(string.find(tostring(errord), "attempt to compare two userdata values", 1, true) ~= nil, true,
    "order error names both operands")

-- 3) no metatable, no metamethod dispatch
assert_eq(u.hasmeta(a), false, "lua_getmetatable reports none")
local ok, err = pcall(function() return a.field end)
assert_eq(ok, false, "indexing a light userdata raises")
assert_eq(string.find(tostring(err), "attempt to index a userdata value", 1, true) ~= nil, true,
    "index error names the type")
assert_eq(u.setmeta(a, { __tostring = function() return "X" end, __index = function() return "X" end }), a,
    "lua_setmetatable leaves the value alone")
assert_eq(u.hasmeta(a), false, "a light userdata still has no metatable")
local okix = pcall(function() return a.field end)
assert_eq(okix, false, "__index is not reachable from a light userdata")

-- 4) tostring
assert_eq(u.tostr(a), "userdata: 0x1", "tostring renders the address")
assert_eq(string.find(tostring(a), "userdata", 1, true) ~= nil, true, "tostring does not crash")

-- 5) concatenation requires a string
local ok2 = pcall(function() return a .. "x" end)
assert_eq(ok2, false, "concat with a light userdata raises")

-- 6) pointer keys: lua_rawsetp/lua_rawgetp and a pushed light userdata key agree
local t = {}
u.rawpset(t, 16, "v16")
assert_eq(u.rawpget(t, 16), "v16", "rawgetp finds what rawsetp stored")
assert_eq(u.lightkeyget(t, 16), "v16", "a pushed light userdata key matches the pointer key")
assert_eq(t[u.mkptr(16)], "v16", "the Lua-side key lookup matches")
assert_eq(u.rawpget(t, 17), nil, "an absent pointer key reads nil")

-- 7) unmapped addresses survive every value operation
local wild = u.mkptr(0xdeadbeef)
assert_eq(wild == u.mkptr(0xdeadbeef), true, "wild pointer equality works")
assert_eq(u.tostr(wild), "userdata: 0xdeadbeef", "wild pointer tostring works")
assert_eq(tostring(wild), "userdata: 0xdeadbeef", "wild pointer tostring via Lua works")
t[wild] = "wild"
assert_eq(t[u.mkptr(0xdeadbeef)], "wild", "wild pointer works as a table key")
assert_eq(#tostring(wild), #"userdata: 0xdeadbeef", "wild pointer string length")

-- 8) GC keeps working with light userdata in reachable places
collectgarbage("collect")
t[wild] = nil
collectgarbage("collect")
collectgarbage("collect")
assert_eq(t[u.mkptr(16)], "v16", "pointer key survives collections")

print("-----")
print("PASS  :", passed)
print("FAIL  :", failed)
print("-----")
os.exit(failed > 0 and 1 or 0)
