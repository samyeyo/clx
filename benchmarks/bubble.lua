local function bubble_sort(size)
    local t = {}
    
    -- Populate table in descending order (worst-case scenario)
    for i = 1, size do
        t[i] = size - i
    end
    
    -- O(N^2) sorting
    for i = 1, size do
        for j = 1, size - i do
            if t[j] > t[j+1] then
                -- Swap
                local temp = t[j]
                t[j] = t[j+1]
                t[j+1] = temp
            end
        end
    end
    
    return t[1], t[size]
end

print("Bubble Sorting 2,000 elements (Worst Case)...")
local first, last = bubble_sort(2000)
print("First: " .. tostring(first) .. " | Last: " .. tostring(last))