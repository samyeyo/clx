local inspect = require("inspect")
assert(type(inspect({1,2,3,a="hello"})) == "string")
print("[PASS] inspect")
