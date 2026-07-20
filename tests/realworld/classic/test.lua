local Object = require("classic")

local Animal = Object:extend()

function Animal:new(name)
    self.name = name
end

local a = Animal("Bob")
assert(a.name == "Bob")

print("[PASS] classic")
