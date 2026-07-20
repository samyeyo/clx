local colors = require("ansicolors")

local s = colors("%{red}hello%{reset}")

assert(type(s) == "string")

print("[PASS] ansicolors")
