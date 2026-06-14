local passed = 0

-- On exporte une simple table avec une fonction
local M = {}

function M.hello(name)
    return "Hello " .. tostring(name) .. "!"
end

return M