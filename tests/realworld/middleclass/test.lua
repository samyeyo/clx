local class = require("middleclass")

local Animal = class("Animal")

function Animal:initialize(name)
    self.name = name
end

local a = Animal("Bob")
assert(a.name == "Bob")

print("[PASS] middleclass")
