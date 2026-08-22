-- Regression: math.frexp() with no arguments must raise an error.

local ok, err = pcall(math.frexp)
assert(not ok and tostring(err):find("bad argument #1 to 'frexp'", 1, true), err)

local m, e = math.frexp(8)
assert(m == 0.5 and e == 4)

print("[OK]    frexp_noarg")
