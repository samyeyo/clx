-- Regression test: R1 GC byte accounting.
--
-- allocated_bytes used to count only object headers, so a table's array buffer,
-- hash entries, bitmap and inline cache were invisible to GC pacing. Churning
-- tables with large arrays therefore never crossed the minor threshold and the
-- heap grew without bound. table_heap_bytes()/account_delta() make the counter
-- track the real side-buffer bytes.

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

local function mk(n)
    local x = {}
    for i = 1, n do
        x[i] = i
    end
    return x[n]
end

local before = collectgarbage("stats")

local sum = 0
for r = 1, 9000 do
    sum = sum + mk(100)
end

local after = collectgarbage("stats")

eq(sum, 900000, "array churn returns the right value")
eq(after.minors > before.minors, true, "array churn crosses the minor GC threshold")

local g = {}
g[1] = 5
g[10] = 7
eq(g[5], nil, "gap in array part stays nil")
eq(g[10], 7, "gap keeps its far value")
eq(mk(100), 100, "churned table value")

collectgarbage("collect")
eq(collectgarbage("count") < 4096, true, "heap is bounded after a full collect")

print(string.format("pass=%d fail=%d", pass, fail))
if fail > 0 then os.exit(1) end
