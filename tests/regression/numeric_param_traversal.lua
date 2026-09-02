-- Regression: optimizer numeric-parameter fixpoint pass must not blow up
-- exponentially on deeply nested code (used to hang clx on tlcli.lua).
-- The traversal previously pushed every node's children twice, doubling
-- visits per nesting level. Also covers param detection in return
-- statements and generic-for bodies, and cross-function propagation.

local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

-- Numeric param buried 30 while-levels deep; traversal must stay linear.
local function deep_nest(i)
    local a = i
    local b = 0
    while a < 10 do
        while a < 20 do
            while a < 30 do
                while a < 40 do
                    while a < 50 do
                        while a < 60 do
                            while a < 70 do
                                while a < 80 do
                                    while a < 90 do
                                        while a < 100 do
                                            while a < 110 do
                                                while a < 120 do
                                                    while a < 130 do
                                                        while a < 140 do
                                                            while a < 150 do
                                                                while a < 160 do
                                                                    while a < 170 do
                                                                        while a < 180 do
                                                                            while a < 190 do
                                                                                while a < 200 do
                                                                                    while a < 210 do
                                                                                        while a < 220 do
                                                                                            while a < 230 do
                                                                                                while a < 240 do
                                                                                                    while a < 250 do
                                                                                                        while a < 260 do
                                                                                                            while a < 270 do
                                                                                                                while a < 280 do
                                                                                                                    while a < 290 do
                                                                                                                        a = a + 1
                                                                                                                        b = b + 2
                                                                                                                    end
                                                                                                                end
                                                                                                            end
                                                                                                        end
                                                                                                    end
                                                                                                end
                                                                                            end
                                                                                        end
                                                                                    end
                                                                                end
                                                                            end
                                                                        end
                                                                    end
                                                                end
                                                            end
                                                        end
                                                    end
                                                end
                                            end
                                        end
                                    end
                                end
                            end
                        end
                    end
                end
            end
        end
    end
    return a + b
end

-- Arithmetic only in a return statement must still mark the param numeric.
local function ret_add(pos)
    return pos + 1
end

-- Arithmetic inside a generic-for body must still mark the param numeric.
local function sum_keys(t)
    local s = 0
    local n = 0
    for _, v in ipairs(t) do
        s = s + v
        n = n + 1
    end
    return s, n
end

-- Propagation target: called with another function's parameter.
local function scale(x)
    return x * 2 + 1
end

local function propagate(a)
    return scale(a) + a
end

assert_eq(deep_nest(0), 870, "deep_nest(0)")
assert_eq(deep_nest(5), 860, "deep_nest(5)")
assert_eq(ret_add(41), 42, "ret_add")
assert_eq(sum_keys({ 1, 2, 3, 4 }), 10, "sum_keys sum")
local _, cnt = sum_keys({ 7, 8, 9 })
assert_eq(cnt, 3, "sum_keys count")
assert_eq(propagate(10), 31, "propagate")

print_summary("REGRESSION_NUMERIC_PARAM_TRAVERSAL")

if failed > 0 then os.exit(1) end
