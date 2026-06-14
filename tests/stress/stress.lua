local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
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

print("----------------- Memory & GC Stress")

local count = 0

for i = 1, 5000 do
    local t = { id = i, data = 99 }
    if t.data == 99 then
        count = count + 1
    end
end

assert_eq(count, 5000, "table allocation loop")

local function heavy_closure_generator()
    local x = 0
    return function()
        x = x + 1
        return x
    end
end

local closure_runs = 0
for i = 1, 1000 do
    local fn = heavy_closure_generator()
    fn()
    if fn() == 2 then
        closure_runs = closure_runs + 1
    end
end

assert_eq(closure_runs, 1000, "closure allocation loop")

print_summary("STRESS")