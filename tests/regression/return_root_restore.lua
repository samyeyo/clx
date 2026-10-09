-- Regression test: a return expression must be evaluated while the function's
-- locals are still rooted on the shadow stack.
--
-- emitReturnStatement restored L->shadow_top to the function-entry value *before*
-- evaluating the returned expression. For `return { f(), l }` that meant f() -- and
-- the create_table that starts the constructor -- ran with `l` unreachable: a
-- collection swept it (array_size := 0, the LTable recycled) while the constructor
-- stored a copy of it. binarytrees hit this as a cyclic tree and an unbounded
-- ItemCheck recursion.

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

local function sweep_now()
    collectgarbage("collect")
    return 0
end

local function victim()
    local l = { 41, 42, 43 }
    sweep_now()
    return { sweep_now(), l }
end

local kept = victim()
eq(kept[2][1], 41, "local survives the collection inside its return expression")
eq(kept[2][2], 42, "middle element intact")
eq(kept[2][3], 43, "tail element intact")

local function nested()
    local a = { 11 }
    local b = { 22 }
    sweep_now()
    return { sweep_now(), a, b }
end

local nk = nested()
eq(nk[2][1], 11, "first local survives")
eq(nk[3][1], 22, "second local survives")

print(string.format("pass=%d fail=%d", pass, fail))
if fail > 0 then os.exit(1) end
