local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "expected:", expected, "actual:", actual)
    end
end

local function print_summary()
    print("-----")
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

-- the child process ends with a live finalizable table and records that its __gc ran
local child_mode = arg and arg[1]
local child_marker = arg and arg[2]

if child_mode then
    local mt = {}
    if child_mode ~= "unmarked" then
        mt.__gc = function()
            local f = io.open(child_marker, "w")
            if f then
                f:write(child_mode)
                f:close()
            end
        end
    end
    local live_table = setmetatable({}, mt)
    local live_ref = live_table
    if child_mode == "os.exit" then
        os.exit(0)
    end
else
    print("----------------- GC finalizer test")

    local count = 0
    local function bump()
        count = count + 1
    end

    do
        local t = setmetatable({}, { __gc = bump })
        t = nil
    end
    collectgarbage("collect")
    assert_eq(count, 1, "table __gc runs when the table is collected")

    collectgarbage("collect")
    assert_eq(count, 1, "table __gc runs only once")

    local cycle = {}
    do
        local a = {}
        local b = {}
        local mt = { __gc = bump }
        setmetatable(a, mt)
        setmetatable(b, mt)
        a.other = b
        b.other = a
        cycle = { a, b }
    end
    cycle = nil
    collectgarbage("collect")
    assert_eq(count, 3, "table __gc runs for both members of a garbage cycle")

    for _, mode in ipairs({ "generational", "incremental" }) do
        local before = count
        do
            local t = setmetatable({}, { __gc = bump })
            t = nil
        end
        collectgarbage(mode)
        collectgarbage("collect")
        assert_eq(count, before + 1, "table __gc runs in " .. mode .. " mode")
    end

    local late = 0
    do
        local mt = {}
        local t = setmetatable({}, mt)
        mt.__gc = function() late = late + 1 end
        t = nil
    end
    collectgarbage("collect")
    assert_eq(late, 0, "adding __gc after the metatable is set does not mark the table")

    local replaced = ""
    do
        local mt = { __gc = function() replaced = "first" end }
        local t = setmetatable({}, mt)
        mt.__gc = function() replaced = "second" end
        t = nil
    end
    collectgarbage("collect")
    assert_eq(replaced, "second", "replacing __gc runs the current metamethod")

    local cleared = 0
    do
        local mt = { __gc = function() cleared = cleared + 1 end }
        local t = setmetatable({}, mt)
        mt.__gc = nil
        t = nil
    end
    collectgarbage("collect")
    assert_eq(cleared, 0, "clearing __gc after marking runs no finalizer")

    local called = 0
    do
        local callable = setmetatable({}, { __call = function(self, obj) called = called + 1 end })
        local t = setmetatable({}, { __gc = callable })
        t = nil
    end
    collectgarbage("collect")
    assert_eq(called, 1, "a callable table as __gc is called")

    local recovered = 0
    do
        local t = setmetatable({}, { __gc = 42 })
        t = nil
    end
    collectgarbage("collect")
    do
        local t = setmetatable({}, { __gc = function() recovered = recovered + 1 end })
        t = nil
    end
    collectgarbage("collect")
    assert_eq(recovered, 1, "a non-callable __gc is reported and the collection completes")

    local survived = true
    do
        local t = setmetatable({}, { __gc = function() error("boom") end })
        t = nil
    end
    collectgarbage("collect")
    assert_eq(survived, true, "an error inside __gc does not escape the collection")

    local marker = os.tmpname()
    local function run_child(mode)
        os.remove(marker)
        -- cmd.exe drops the outer pair of quotes when the command starts with one, so on Windows the whole
        -- line is wrapped again (a quoted program plus quoted arguments would otherwise be mangled)
        local command = string.format('"%s" %s "%s"', arg[0], mode, marker)
        if package.config:sub(1, 1) == "\\" then
            command = '"' .. command .. '"'
        end
        os.execute(command)
        local f = io.open(marker, "r")
        local text = f and f:read("a")
        if f then
            f:close()
        end
        os.remove(marker)
        return text
    end

    assert_eq(run_child("return"), "return", "table __gc runs when the state closes")
    assert_eq(run_child("os.exit"), "os.exit", "table __gc runs on os.exit")
    assert_eq(run_child("unmarked"), nil, "table without a marked metatable is not finalized at close")

    print_summary()
end
