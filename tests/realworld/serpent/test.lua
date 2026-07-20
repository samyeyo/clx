local serpent = require("serpent")
assert(type(serpent.dump({x=42})) == "string")
print("[PASS] serpent")
