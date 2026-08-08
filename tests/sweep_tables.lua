local n = 10000
if arg and arg[1] then n = tonumber(arg[1]) end
local t = {}
for i = 1, n do
    t[i] = {i, i*2, i*3}
end
t = nil
collectgarbage("collect")
print("[OK] plain table full-sweep survived, count=" .. tostring(collectgarbage("count")))
