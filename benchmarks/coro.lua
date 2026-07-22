local N = 5000000

local co = coroutine.create(function()
    for i = 1, N do
        coroutine.yield(i)
    end
end)

local sum = 0
for i = 1, N do
    local ok, val = coroutine.resume(co)
    sum = sum + val
end

print("sum = " .. string.format("%.0f", sum))
