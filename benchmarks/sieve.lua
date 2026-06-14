local function sieve(limit)
    local is_prime = {}
    local count = 0
    
    for i = 1, limit do
        is_prime[i] = true
    end
    
    for p = 2, limit do
        if is_prime[p] then
            count = count + 1
            for i = p * p, limit, p do
                is_prime[i] = false
            end
        end
    end
    
    return count
end

print("Calculating primes under 2,000,000...")
local res = sieve(2000000)
print("Total primes found: " .. tostring(res))