local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected: ", expected, " Got: ", actual)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- repeat/until sees body locals")

-- 1. Plain body-local referenced in the until condition: the loop exits as soon
-- as the body-local reaches 5, so the condition must bind to the local, not a global.
local function plain_local()
    local n = 0
    repeat
        local v = n + 1
        n = v
    until v >= 5
    return n
end
assert_eq(plain_local(), 5, "plain body-local visible in until")

-- 2. a shadow-redeclared body-local: the innermost redeclaration (value 2) must be the
-- one the until sees. This used to close the redecl scope before the condition and fall
-- back to the global (nil), so the loop iterated past the shadow's scope.
local runs = 0
local function shadowed_local()
    local i = 0
    repeat
        local x = 100
        local x = 2 -- shadow redeclaration
        runs = runs + 1
        i = i + 1
    until i > 2 or x == 2 -- x resolves to the inner local (2) => single iteration
    return i
end
assert_eq(shadowed_local(), 1, "shadowed body-local visible in until")
assert_eq(runs, 1, "shadowed until bound to inner local (1 iteration)")

-- 3. Body locals disappear after the loop: `x` after `until` resolves to a global (nil),
-- not to either of the loop's locals. This also checks the deferred scopes actually close.
local function scope_closes()
    local i = 0
    repeat
        local x = 100
        local x = 2
        i = i + 1
    until i > 2 or x == 2
    return x
end
assert_eq(scope_closes(), nil, "repeat body locals not visible after loop")

-- 4. A repeat nested inside a while loop keeps its own enclosing scope intact.
local function nested()
    local total = 0
    for j = 1, 3 do
        local k = 0
        repeat
            local inc = total + 1
            total = total + inc
            k = k + 1
        until k >= j -- uses the loop var and a repeat-local together
    end
    return total
end
assert_eq(nested(), 63, "nested repeat uses loop var + body local in until")

print_summary("REPEAT_UNTIL_SCOPE")

if failed > 0 then
    os.exit(1)
end