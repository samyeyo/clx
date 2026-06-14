local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", actual)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- 1. The Parallel Swap")

local x, y = 10, 20
x, y = y, x

assert_eq(x, 20, "Parallel swap x")
assert_eq(y, 10, "Parallel swap y")

local a, b, c = 1, 2, 3
a, b, c = c, a, b

assert_eq(a, 3, "Parallel swap a")
assert_eq(b, 1, "Parallel swap b")
assert_eq(c, 2, "Parallel swap c")

print("\n----------------- 2. Short-Circuiting Objects")

local obj1 = { name = "first" }
local obj2 = { name = "second" }

local res1 = obj1 or obj2
assert_eq(res1.name, "first", "OR returns first truthy object")

local res2 = false or obj2
assert_eq(res2.name, "second", "OR skips falsy and returns object")

local res3 = obj1 and obj2
assert_eq(res3.name, "second", "AND returns second truthy object")

local res4 = nil and obj1
assert_eq(res4, nil, "AND short-circuits on nil")

print("\n----------------- 3. Method Call Single-Evaluation")

local eval_count = 0
local function get_obj()
    eval_count = eval_count + 1
    return {
        val = 42,
        get_val = function(self)
            return self.val
        end
    }
end

local v = get_obj():get_val()
assert_eq(v, 42, "Method call returns correct value")
assert_eq(eval_count, 1, "Method call evaluates object EXACTLY once")

print("\n----------------- 4. The Ambiguous Parenthesis")

local call_count = 0
local function trap()
    call_count = call_count + 1
    return function(n) return n * 2 end
end

local result = trap()
(5)

assert_eq(result, 10, "Newline before parenthesis parsed as function call")
assert_eq(call_count, 1, "LHS evaluated exactly once")

print_summary("EDGE_CASES")