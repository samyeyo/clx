local function ackermann(m, n)
    if m == 0 then 
        return n + 1 
    end
    if m > 0 and n == 0 then 
        return ackermann(m - 1, 1) 
    end
    return ackermann(m - 1, ackermann(m, n - 1))
end

print("Calculating Ackermann(3, 8)...")
local res = ackermann(3, 8)
print("Result: " .. tostring(res))