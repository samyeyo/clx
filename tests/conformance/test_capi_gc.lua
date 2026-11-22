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

local function print_summary()
    print("-----")
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

local g = require("capi_gc_mod")

-- the child process holds live userdata until the state closes, its __gc writes the marker file
local child_mode = arg and arg[1]
local child_marker = arg and arg[2]

if child_mode then
    local live
    if child_mode == "unmarked" then
        live = { g.new_open("unmarked") }
    else
        live = { g.new(child_mode) }
    end
    g.marker(child_marker)
    if child_mode == "os.exit" then
        os.exit(0)
    end
else
    print("----------------- C API userdata __gc test")

    local function log_has(name)
        return string.find(g.log(), name, 1, true) ~= nil
    end

-- 1) collected userdata are finalized once, each with its own payload
    g.reset()
    do
        local a = g.new("a")
        local b = g.new("b")
        a, b = nil, nil
    end
    collectgarbage("collect")
    assert_eq(g.count(), 2, "userdata __gc runs for collected userdata")
    assert_eq(log_has("a") and log_has("b"), true, "userdata __gc sees each payload")

    collectgarbage("collect")
    collectgarbage("collect")
    assert_eq(g.count(), 2, "userdata __gc runs only once")

-- 2) userdata dropped together with the table holding them
    g.reset()
    do
        local holder = { g.new("nested1"), g.new("nested2"), extra = g.new("nested3") }
        holder = nil
    end
    collectgarbage("collect")
    assert_eq(g.count(), 3, "userdata __gc runs for userdata reachable only from dropped tables")

-- 3) every collection mode
    for _, mode in ipairs({ "generational", "incremental" }) do
        g.reset()
        do
            local u = g.new(mode)
            u = nil
        end
        collectgarbage(mode)
        collectgarbage("collect")
        assert_eq(g.count(), 1, "userdata __gc runs in " .. mode .. " mode")
    end

-- 4) __gc timing follows stock: only a metatable that already had __gc marks the userdata
    g.reset()
    do
        local u = g.new_open("late")
        g.arm(u)
        u = nil
    end
    collectgarbage("collect")
    assert_eq(g.count(), 0, "adding __gc after the metatable is set does not mark the userdata")

    g.reset()
    do
        local u = g.new("orig")
        g.arm_alt(u)
        u = nil
    end
    collectgarbage("collect")
    assert_eq(g.log(), "orig!", "replacing __gc runs the current metamethod")

    g.reset()
    do
        local u = g.new("gone")
        g.disarm(u)
        u = nil
    end
    collectgarbage("collect")
    assert_eq(g.count(), 0, "clearing __gc after marking runs no finalizer")

-- 5) __gc accepts anything callable, like stock, and reports a non-callable one without dying
    g.reset()
    local hits = 0
    local callable = setmetatable({}, { __call = function(self, obj) hits = hits + 1 end })
    do
        local u = g.new("callable")
        g.arm_value(u, callable)
        u = nil
    end
    collectgarbage("collect")
    assert_eq(hits, 1, "a callable table as __gc is called for userdata")

    g.reset()
    do
        local u = g.new("broken")
        g.arm_value(u, 42)
        u = nil
    end
    collectgarbage("collect")
    assert_eq(g.count(), 0, "a non-callable __gc is reported and the collection completes")

-- 6) userdata still alive when the state closes
    local marker = os.tmpname()
    local function run_child(mode)
        os.remove(marker)
        os.execute(string.format('"%s" %s "%s"', arg[0], mode, marker))
        local f = io.open(marker, "r")
        local text = f and f:read("a")
        if f then
            f:close()
        end
        os.remove(marker)
        return text
    end

    g.reset()
    assert_eq(run_child("return"), "return", "userdata __gc runs when the state closes")
    assert_eq(run_child("os.exit"), "os.exit", "userdata __gc runs on os.exit")
    assert_eq(run_child("unmarked"), nil, "userdata without a marked metatable is not finalized at close")

    print_summary()
end
