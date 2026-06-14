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

print("----------------- Garbage Collection (Pool Semantics)")

local mem_initial = collectgarbage("count")

local function allocate_garbage()
    local t = {}
    for i = 1, 10000 do
        t[i] = { data = i }
    end
end

allocate_garbage()
local mem_peak_1 = collectgarbage("count")

assert_eq(mem_peak_1 > mem_initial and 1 or 0, 1, "pool capacity expands on first allocation")

collectgarbage("collect")

allocate_garbage()
local mem_peak_2 = collectgarbage("count")

assert_eq(mem_peak_2, mem_peak_1, "pool reuses freed slots without expanding")

print_summary("GARBAGE COLLECTION")