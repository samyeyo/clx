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

print("----------------- Error Handling & Protected Calls")

local ok1, res1 = pcall(function(a, b) return a * b end, 6, 7)
assert_eq(ok1, true, "pcall success - returns true")
assert_eq(res1, 42, "pcall success - forwards return values")

local ok2, err2 = pcall(function()
    error("custom abort", 0)
end)
assert_eq(ok2, false, "pcall error - traps explicit error()")
assert_eq(err2, "custom abort", "pcall error - exact payload returned")

local ok3, err3 = pcall(function()
    local a = nil
    return a + 5
end)
assert_eq(ok3, false, "pcall engine exception - traps C++ runtime error")
assert_eq(type(err3), "string", "pcall engine exception - returns formatted string")

local ok4, res4 = xpcall(function() return 99 end, function() return "failed" end)
assert_eq(ok4, true, "xpcall success - returns true")
assert_eq(res4, 99, "xpcall success - ignores handler on success")

local ok5, err5 = xpcall(
    function()
        error("raw_payload", 0)
    end,
    function(err)
        return "INTERCEPTED: " .. tostring(err)
    end
)
assert_eq(ok5, false, "xpcall error - traps error")
assert_eq(err5, "INTERCEPTED: raw_payload", "xpcall error - handler modifies output")

local ok6, err6 = xpcall(
    function()
        error("first crash")
    end,
    function(err)
        error("handler crashed too!")
    end
)
assert_eq(ok6, false, "xpcall broken handler - traps double fault")
assert_eq(err6, "error in error handling", "xpcall broken handler - returns hardcoded fallback")

print_summary("EXCEPTIONS")