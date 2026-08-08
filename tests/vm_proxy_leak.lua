local function rss_kb()
    local f = io.open("/proc/self/status", "r")
    if not f then return -1 end
    local line = f:read("*l")
    while line do
        local kb = string.match(line, "^VmRSS:%s*(%d+)")
        if kb then f:close() return tonumber(kb) end
        line = f:read("*l")
    end
    f:close()
    return -1
end

local n = 20000
if arg and arg[1] then n = tonumber(arg[1]) end

collectgarbage("collect")
local before_rss = rss_kb()
local before_cg = collectgarbage("count")

for i = 1, n do
    local fn = load("return " .. i)
end

collectgarbage("collect")
local after_rss = rss_kb()
local after_cg = collectgarbage("count")

print(string.format("rss=%dKB->%dKB (delta %dKB) cg=%.1fKB->%.1fKB (delta %.1fKB) calls=%d",
    before_rss, after_rss, after_rss - before_rss,
    before_cg, after_cg, after_cg - before_cg, n))
