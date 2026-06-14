-- Hash table benchmark
-- Tests table insert and lookup performance

local N = tonumber(arg and arg[1]) or 100000

local tbl = {}
for i = 1, N do
    tbl["key_" .. i] = i * 2
end

local sum = 0
for i = 1, N do
    sum = sum + (tbl["key_" .. i] or 0)
end

print(sum)