local dkjson = require("dkjson")

local f = assert(io.open("canada.json", "r"))
local raw = f:read("*a")
f:close()

local data = assert(dkjson.decode(raw))

-- Walk every coordinate pair in every polygon and compute
-- the bounding box of the entire dataset.
local min_x, max_x =  math.huge, -math.huge
local min_y, max_y =  math.huge, -math.huge
local total_points = 0

for _, feature in ipairs(data.features) do
    local geom = feature.geometry
    -- canada.json uses Polygon and MultiPolygon
    local rings = geom.coordinates
    if geom.type == "Polygon" then
        rings = { rings }
    end
    for _, polygon in ipairs(rings) do
        for _, ring in ipairs(polygon) do
            for _, pt in ipairs(ring) do
                local x, y = pt[1], pt[2]
                if x < min_x then min_x = x end
                if x > max_x then max_x = x end
                if y < min_y then min_y = y end
                if y > max_y then max_y = y end
                total_points = total_points + 1
            end
        end
    end
end

print(string.format("features:     %d", #data.features))
print(string.format("total points: %d", total_points))
print(string.format("bbox x:       [%.6f, %.6f]", min_x, max_x))
print(string.format("bbox y:       [%.6f, %.6f]", min_y, max_y))