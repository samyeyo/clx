local mymod = require("mymod")

if type(mymod) == "table" and type(mymod.hello) == "function" then
    print("[OK]   Module 'mymod' successfully loaded via require")
    
    local res = mymod.hello("CLX")
    if res == "Hello CLX!" then
        print("[OK]   Module function executed correctly: " .. res)
    else
        print("[FAIL] Unexpected function return: " .. tostring(res))
    end
else
    print("[FAIL] Failed to load module 'mymod' as a table")
end