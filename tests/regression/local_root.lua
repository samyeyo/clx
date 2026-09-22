-- Regression test: shadow-stack rooting of block locals.
--
-- Locals that are never reassigned are rooted through a private snapshot slot
-- (codegen.cpp: emit_local_root) instead of through their own address, so the local
-- itself is never addressed and can stay in a register on hot paths. The GC only ever
-- reads shadow slots, so a snapshot is equivalent -- but only as long as the variable
-- is never assigned again. These cases pin down that equivalence.

local pass, fail = 0, 0
local function eq(actual, expected, name)
    if actual == expected then
        pass = pass + 1
        print("[OK] " .. name)
    else
        fail = fail + 1
        print("[FAIL] " .. name .. ": got " .. tostring(actual) .. ", expected " .. tostring(expected))
    end
end

-- a never-reassigned local alive across a swap loop
local function swap_loop()
    local t = {}
    for i = 1, 200 do t[i] = i end
    for i = 1, 100 do
        local tmp = t[i]
        t[i] = t[201 - i]
        t[201 - i] = tmp
    end
    local sum = 0
    for i = 1, 200 do sum = sum + t[i] end
    return sum
end
eq(swap_loop(), 20100, "never-reassigned local swaps correctly")

-- a reassigned local still takes the direct (unsnapshotted) root
local function reassigned()
    local x = 1
    local acc = 0
    for i = 1, 10 do
        x = x + i
        acc = acc + x
    end
    return x, acc
end
local rx, racc = reassigned()
eq(rx, 56, "reassigned local keeps its value")
eq(racc, 230, "reassigned accumulator")

-- the snapshot must keep the object alive when it is the only reference
local function only_reference()
    local holder = { obj = { 4242, "alive" } }
    local only = holder.obj
    holder.obj = nil
    collectgarbage("collect")
    local churn = 0
    for i = 1, 2000 do
        local junk = { i, i + 1, i + 2, i + 3 }
        churn = churn + junk[1]
    end
    return only[1], only[2], churn ~= 0
end
local ov, os_, oc = only_reference()
eq(ov, 4242, "only-reference local survives a full GC")
eq(os_, "alive", "only-reference local's string survives a full GC")
eq(oc, true, "allocation churn ran")

-- same, but the local is re-created every iteration
local function per_iteration()
    local n = 0
    for i = 1, 300 do
        local box = { payload = { i } }
        local only = box.payload
        box.payload = nil
        if i % 50 == 0 then collectgarbage("collect") end
        n = n + only[1]
    end
    return n
end
eq(per_iteration(), 45150, "per-iteration only-reference local survives a full GC")

-- a local captured by a closure created each iteration
local function closures()
    local fns = {}
    for i = 1, 5 do
        local v = i * 10
        fns[i] = function() return v end
    end
    return fns[1]() + fns[5]()
end
eq(closures(), 60, "per-iteration closure captures its own local")

-- a never-reassigned local captured by a closure
local function captured_table()
    local t = { 7, 8, 9 }
    local g = function() return t[2] end
    collectgarbage("collect")
    return g()
end
eq(captured_table(), 8, "captured table local")

-- a for-loop variable shadowing a never-reassigned local of the same name
local function shadowed_for()
    local x = 5
    local acc = 0
    for x = 1, 3 do acc = acc + x end
    return acc, x
end
local sa, sx = shadowed_for()
eq(sa, 6, "shadowing for-variable sums correctly")
eq(sx, 5, "outer local survives shadowing")

-- the same local name declared in two sibling blocks
local function sibling_blocks()
    local r = 0
    do
        local tmp = { 1, 2 }
        r = r + tmp[1] + tmp[2]
    end
    do
        local tmp = { 3, 4 }
        r = r + tmp[1] + tmp[2]
    end
    return r
end
eq(sibling_blocks(), 10, "same name in sibling blocks")

-- a goto crossing a local declaration leaves it uninitialised
local function goto_crossed()
    goto skip
    local t = { 1, 2, 3 }
    ::skip::
    for i = 1, 3 do
        if not pcall(function() return t[i] end) then
            return "err"
        end
    end
    return "noerr"
end
eq(goto_crossed(), "err", "goto-crossed local is not hoisted")

-- a parameter that is the only reference to its object across a GC
local function param_only_reference(arg)
    local holder = { ref = arg }
    arg = nil
    collectgarbage("collect")
    local churn = 0
    for i = 1, 1500 do
        local junk = { i, i + 1 }
        churn = churn + junk[1]
    end
    return holder.ref[1], holder.ref[2], churn ~= 0
end
local pv, ps, pc = param_only_reference({ 31337, "param" })
eq(pv, 31337, "only-reference parameter survives a full GC")
eq(ps, "param", "only-reference parameter's string survives a full GC")
eq(pc, true, "parameter allocation churn ran")

-- a parameter that is reassigned in the body still takes the direct root
local function param_reassigned(arg)
    local first = arg
    arg = 99
    return first, arg
end
local pr1, pr2 = param_reassigned(7)
eq(pr1, 7, "reassigned parameter keeps its first value")
eq(pr2, 99, "reassigned parameter takes the new value")

-- a generic-for variable that is the only reference to its object across a GC
local function forvar_only_reference()
    local list = { { 555, "forvar" } }
    for _, v in ipairs(list) do
        list[1] = nil
        collectgarbage("collect")
        local churn = 0
        for i = 1, 500 do
            local junk = { i }
            churn = churn + junk[1]
        end
        return v[1], v[2], churn > 0
    end
    return nil, nil, false
end
local fv, fs, fc = forvar_only_reference()
eq(fv, 555, "only-reference loop variable survives a full GC")
eq(fs, "forvar", "only-reference loop variable's string survives a full GC")
eq(fc, true, "loop variable allocation churn ran")

-- a parameter passed on to another function keeps its value
local function takes_table(t)
    return t.a
end
local function param_passed_on(x)
    local y = takes_table(x)
    return x.b, y
end
local pp1, pp2 = param_passed_on({ a = "A", b = "B" })
eq(pp1, "B", "parameter used as a call argument")
eq(pp2, "A", "parameter forwarded into a function")

-- the <close> attribute still works alongside the root
local function close_attr()
    local closed = false
    do
        local h <close> = setmetatable({}, { __close = function() closed = true end })
        if h == nil then return "bad" end
    end
    return closed
end
eq(close_attr(), true, "close attribute")

print(string.format("pass=%d fail=%d", pass, fail))
if fail > 0 then os.exit(1) end
