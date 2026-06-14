local WIDTH = 40
local HEIGHT = 20
local GENERATIONS = 300

local function create_grid(...)
    local grid = {}
    for y = 1, HEIGHT do
        grid[y] = {}
        for x = 1, WIDTH do
            grid[y][x] = 0
        end
    end
    local args = {...}
    for i = 1, #args, 2 do
        local x, y = args[i], args[i+1]
        if x and y then grid[y][x] = 1 end
    end
    return grid
end

local function count_neighbors(grid, x, y)
    local count = 0
    for dy = -1, 1 do
        for dx = -1, 1 do
            if not (dx == 0 and dy == 0) then
                local nx, ny = x + dx, y + dy
                if nx >= 1 and nx <= WIDTH and ny >= 1 and ny <= HEIGHT then
                    if grid[ny][nx] == 1 then
                        count = count + 1
                    end
                end
            end
        end
    end
    return count
end

local function update(grid)
    local new_grid = create_grid()
    for y = 1, HEIGHT do
        for x = 1, WIDTH do
            local neighbors = count_neighbors(grid, x, y)
            local state = grid[y][x]
            if state == 1 and (neighbors == 2 or neighbors == 3) then
                new_grid[y][x] = 1
            elseif state == 0 and neighbors == 3 then
                new_grid[y][x] = 1
            else
                new_grid[y][x] = 0
            end
        end
    end
    return new_grid
end

local function print_grid(grid, gen)
    print("\27[H") -- ANSI Clear Screen
    print("Generation: " .. gen)
    for y = 1, HEIGHT do
        local line = ""
        for x = 1, WIDTH do
            line = line .. (grid[y][x] == 1 and "#" or ".")
        end
        print(line)
    end
end

-- Initialize with a Glider
local current_grid = create_grid(
    2, 1,
    3, 2,
    1, 3, 2, 3, 3, 3
)

print("Starting Conway's Game of Life Benchmark...")

for g = 1, GENERATIONS do
    current_grid = update(current_grid)
    -- Uncomment the line below to visualize (slows down benchmark)
    -- print_grid(current_grid, g)
end

print("Benchmark Complete: " .. GENERATIONS .. " generations simulated.")