-- Array sum benchmark
-- Simple loop over array elements

local N = tonumber(arg and arg[1]) or 15000000

local arr = {}
for i = 1, N do
    arr[i] = i
end

local sum = 0
for i = 1, N do
    sum = sum + arr[i]
end

print(sum)