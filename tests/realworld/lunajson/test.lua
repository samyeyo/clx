local json = require("lunajson")
local t = json.decode('{"x":42}')
assert(t.x == 42)
print("[PASS] lunajson")
