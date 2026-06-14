-- JSON encoding benchmark (no table.concat dependency)
-- Encode a structured table to JSON-like string

local function enc(v, depth)
    depth = depth or 0
    local tv = type(v)
    if tv == "nil" then return "null"
    elseif tv == "boolean" then return v and "true" or "false"
    elseif tv == "number" then return tostring(v)
    elseif tv == "string" then return "'" .. v .. "'"
    elseif tv == "table" then
        local result = "{"
        local first = true
        for k, vv in pairs(v) do
            if not first then result = result .. "," end
            first = false
            result = result .. enc(k, depth + 1) .. ":" .. enc(vv, depth + 1)
        end
        return result .. "}"
    end
    return "null"
end

local N = 8000
local t = {}
for i = 1, N do
    t[i] = { id = i, val = i * 2, name = "obj_" .. i }
end

local json = enc(t)
print(#json)
