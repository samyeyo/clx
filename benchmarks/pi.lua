local function estimate_pi(iterations)
    local inside = 0
    
    for i = 1, iterations do
        -- Custom deterministic pseudo-random generator
        local x = (math.fmod(i * 214013 + 2531011, 32768) / 32768.0) * 2.0 - 1.0
        local y = (math.fmod(i * 1103515245 + 12345, 32768) / 32768.0) * 2.0 - 1.0
        
        if (x*x + y*y) <= 1.0 then
            inside = inside + 1
        end
    end
    
    return (inside / iterations) * 4.0
end

print("Monte Carlo Pi (1,000,000 iterations)...")
local res = estimate_pi(1000000)
print("Result: " .. tostring(res))