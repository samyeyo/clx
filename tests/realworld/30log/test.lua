local class = require("30log")

local Animal = class("Animal")

function Animal:init(name)
    self.name = name
end

local a = Animal("Bob")
assert(a.name == "Bob")

print("[PASS] 30log")
