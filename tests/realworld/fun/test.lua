local fun = require("fun")

local sum = fun.iter({1,2,3}):sum()

assert(sum == 6)

print("[PASS] fun")
