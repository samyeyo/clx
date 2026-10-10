local fails = 0
local function eq(a, b, n)
    if a == b then print("[OK]   " .. n)
    else fails = fails + 1; print("[FAIL] " .. n .. " expected=" .. tostring(b) .. " got=" .. tostring(a)) end
end

-- 1. plain integer sort through the fused path
local function bsort(t)
    local n = #t
    for i = 1, n do
        for j = 1, n - i do
            if t[j] > t[j + 1] then
                local tmp = t[j]
                t[j] = t[j + 1]
                t[j + 1] = tmp
            end
        end
    end
    return t
end

local a = {}
for i = 1, 60 do a[i] = (i * 37) % 61 end
bsort(a)
local sorted = true
for i = 2, 60 do if a[i - 1] > a[i] then sorted = false end end
eq(sorted, true, "int array sorted")
eq(a[1], 1, "first element")
eq(a[60], 60, "last element")

-- 2. string elements exercise the write barrier on the fused stores
local s = {}
for i = 1, 40 do s[i] = "item" .. ((i * 13) % 41) end
bsort(s)
local sok = true
for i = 2, 40 do if s[i - 1] > s[i] then sok = false end end
eq(sok, true, "string array sorted")
eq(#s, 40, "string array length preserved")

-- 3. swap with a nil (out-of-array) read must take the fallback
local h = { 5, 4, 3 }
local n = #h
local got = {}
for j = 1, n do
    local tmp = h[j]
    h[j] = h[j + 1]
    h[j + 1] = tmp
    got[j] = tostring(h[1]) .. "," .. tostring(h[2]) .. "," .. tostring(h[3]) .. "," .. tostring(h[4])
end
eq(got[1], "4,5,3,nil", "nil-slot swap fallback (1)")
eq(got[2], "4,3,5,nil", "nil-slot swap fallback (2)")
eq(got[3], "4,3,nil,5", "nil-slot swap fallback (3)")

-- 4. temp used afterwards must NOT be fused away
local u = { 3, 1, 2 }
local kept
do
    local tmp = u[1]
    u[1] = u[2]
    u[2] = tmp
    kept = tmp
end
eq(kept, 3, "temp survives when referenced later")
eq(u[1], 1, "array swapped with live temp")
eq(u[2], 3, "array swapped with live temp (2)")

-- 5. third statement targeting a DIFFERENT index is not a swap and must not fuse
local v = { 1, 2, 3, 4 }
do
    local tmp = v[1]
    v[1] = v[2]
    v[3] = tmp
end
eq(v[1], 2, "non-swap variant: first slot")
eq(v[2], 2, "non-swap variant: second slot untouched")
eq(v[3], 1, "non-swap variant: third slot")

-- 6. barrier stress: swap tables while the GC runs, then verify every payload
local cells = {}
for i = 1, 80 do cells[i] = { id = i, payload = string.rep("x", i) } end
for round = 1, 40 do
    for j = 1, 79 do
        if cells[j].id > cells[j + 1].id then
            local tmp = cells[j]
            cells[j] = cells[j + 1]
            cells[j + 1] = tmp
        end
    end
    if round % 10 == 0 then collectgarbage("collect") end
end
local intact = true
for i = 1, 80 do
    if cells[i].id ~= i or cells[i].payload ~= string.rep("x", i) then intact = false end
end
eq(intact, true, "GC-object payloads survive swapping + collection")

-- 7. single-element and equal-index degenerate cases
local one = { 9 }
do
    local tmp = one[1]
    one[1] = one[2]
    one[2] = tmp
end
eq(tostring(one[1]), "nil", "degenerate single-element swap")
eq(one[2], 9, "degenerate single-element swap (2)")

print(fails == 0 and "ALL_OK" or ("FAILS=" .. fails))
