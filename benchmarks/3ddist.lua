local function distance_test(iterations)
    local total = 0.0
    
    for i = 1, iterations do
        -- Generate deterministic pseudo-random coordinates
        local x = i % 10
        local y = (i * 2) % 15
        local z = (i * 3) % 20
        
        -- Calculate 3D distance from origin
        local dist = math.sqrt(x*x + y*y + z*z)
        
        -- Accumulate a checksum to prevent C++ from dead-code-eliminating the loop
        total = total + dist
    end
    
    return total
end

print("Running 2,000,000 math.sqrt iterations...")
local res = distance_test(2000000)
print("Checksum: " .. tostring(res))