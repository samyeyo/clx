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

local function assert_true(cond, name)
    if cond then
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

print("----------------- package.path / searchers / searchpath (plain AOT build)")

assert_true(type(package.path) == "string", "package.path is a string")
assert_true(type(package.cpath) == "string", "package.cpath is a string")
assert_true(#package.path > 0, "package.path is not empty")
assert_true(package.path:find("?.lua", 1, true) ~= nil, "package.path contains ?.lua template")
assert_true(type(package.searchers) == "table", "package.searchers is a table")
assert_true(type(package.searchers[1]) == "function", "searchers[1] is the preload searcher")
assert_true(type(package.searchers[2]) == "function", "searchers[2] is the Lua file searcher")
assert_true(type(package.searchers[3]) == "function", "searchers[3] is the C searcher")
assert_true(package.searchers[4] == nil, "searchers has exactly 3 entries")
assert_true(type(package.searchpath) == "function", "package.searchpath is a function")

------------------ searchpath positive: mymod.lua lives in conformance/ next to this test (suite cwd = tests/)
local found = package.searchpath("mymod", "./conformance/?.lua")
assert_eq(found, "./conformance/mymod.lua", "searchpath finds existing file")

------------------ searchpath negative: nil + aggregated message
local miss, msg = package.searchpath("no_such_module_xyz", "./?.lua;./?.luac")
assert_true(miss == nil, "searchpath returns nil on miss")
assert_true(type(msg) == "string", "searchpath returns an error message on miss")
assert_true(type(msg) == "string" and msg:find("no file", 1, true) ~= nil, "message lists probed files")
assert_true(type(msg) == "string" and msg:find("no_such_module_xyz.lua", 1, true) ~= nil, "message names the module")

------------------ searchpath custom separator: 'a.b' with sep ':' must not substitute
local sep_found = package.searchpath("mymod", "./conformance/?.lua", ".")
assert_eq(sep_found, "./conformance/mymod.lua", "searchpath explicit sep")

------------------ require in a plain AOT binary must not load files.
-- Extend package.path so mymod.lua is findable; the searcher must then report the --dynamic requirement.
package.path = package.path .. ";./conformance/?.lua"
local ok, rerr = pcall(require, "mymod")
assert_true(ok == false, "require of a file module fails without --dynamic")
assert_true(type(rerr) == "string", "require failure carries a message")
assert_true(
    type(rerr) == "string" and rerr:find("--dynamic", 1, true) ~= nil, "failure message suggests compiling with --dynamic")

------------------ require of a module missing everywhere aggregates all searcher messages
local ok2, rerr2 = pcall(require, "no_such_module_xyz")
assert_true(ok2 == false, "require of unknown module fails")
assert_true(
    type(rerr2) == "string" and rerr2:find("no field package.preload", 1, true) ~= nil,
    "aggregated message includes preload searcher result")
assert_true(
    type(rerr2) == "string" and rerr2:find("no file", 1, true) ~= nil, "aggregated message includes file searcher result")

------------------ require still resolves static modules via preload (multi-file AOT)
-- handled by run.sh's package.lua + mymod.lua combined build; here verify loaded registry shape
assert_true(type(package.loaded) == "table", "package.loaded is a table")
assert_true(package.loaded.package == package, "package registers itself in package.loaded")

print_summary("package searchers (AOT)")
if failed > 0 then
    error("REGRESSION: " .. failed .. " failures")
end
