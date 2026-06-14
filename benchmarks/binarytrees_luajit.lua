-- Binary trees benchmark — LuaJIT version (uses 2^ instead of <<)
-- https://benchmarksgame-team.pages.debian.net/benchmarksgame/description/binarytrees.html

local function BottomUpTree(depth)
    if depth > 0 then
        depth = depth - 1
        local left  = BottomUpTree(depth)
        local right = BottomUpTree(depth)
        return { left, right }
    else
        return { false, false }
    end
end

local function ItemCheck(tree)
    if tree[1] then
        return 1 + ItemCheck(tree[1]) + ItemCheck(tree[2])
    else
        return 1
    end
end

local function Stress(mindepth, maxdepth, depth)
    local iterations = 2 ^ (maxdepth - depth + mindepth)
    local check = 0
    for _ = 1, iterations do
        local t = BottomUpTree(depth)
        check = check + ItemCheck(t)
    end
    return { iterations, check }
end

N = N or 10

local mindepth = 4
local maxdepth = math.max(mindepth + 2, N)

do
    local stretchdepth = maxdepth + 1
    local stretchtree = BottomUpTree(stretchdepth)
    print(string.format("stretch tree of depth %d\t check: %d",
        stretchdepth, ItemCheck(stretchtree)))
end

local longlivedtree = BottomUpTree(maxdepth)

for depth = mindepth, maxdepth, 2 do
    local r = Stress(mindepth, maxdepth, depth)
    local iterations = r[1]
    local check      = r[2]
    print(string.format("%d\t trees of depth %d\t check: %d",
        iterations, depth, check))
end

print(string.format("long lived tree of depth %d\t check: %d",
    maxdepth, ItemCheck(longlivedtree)))
