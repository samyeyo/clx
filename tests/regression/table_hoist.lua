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

local function assert_true(val, name)
    if val then
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

print("----------------- Hoisted table-local header pointer")

-- for loop read/write (non-vectorizable table: string field disqualifies numeric array)
local function test_for_read_write()
    local t = {}
    t.name = "x"
    for i = 1, 10 do
        t[i] = i * 2
    end
    local sum = 0
    for i = 1, 10 do
        sum = sum + t[i]
    end
    assert_eq(sum, 110, "for read/write sum")
    assert_eq(t.name, "x", "for string field preserved")
end
test_for_read_write()

-- table declared inside the loop is fresh each iteration and must not be hoisted
local function test_inside_loop_decl()
    local total = 0
    for i = 1, 5 do
        local u = {}
        u[i] = i * 10
        total = total + u[i]
    end
    assert_eq(total, 150, "inside-loop fresh table")
end
test_inside_loop_decl()

-- while loop
local function test_while()
    local t = {}
    t.tag = 0
    local n = 0
    while n < 10 do
        n = n + 1
        t[n] = n
    end
    local sum = 0
    for i = 1, 10 do
        sum = sum + t[i]
    end
    assert_eq(sum, 55, "while write/read")
end
test_while()

-- repeat loop
local function test_repeat()
    local t = {}
    t.tag = 0
    local k = 0
    repeat
        k = k + 1
        t[k] = k
    until k == 10
    local sum = 0
    for i = 1, 10 do
        sum = sum + t[i]
    end
    assert_eq(sum, 55, "repeat write/read")
end
test_repeat()

-- nested loops share the same outer table
local function test_nested()
    local t = {}
    t.tag = 0
    for i = 1, 3 do
        for j = 1, 3 do
            t[i * 3 + j] = i * j
        end
    end
    local sum = 0
    for i = 1, 3 do
        for j = 1, 3 do
            sum = sum + t[i * 3 + j]
        end
    end
    assert_eq(sum, 36, "nested loop sum")
end
test_nested()

-- reassigned table must stay correct (analysis excludes it)
local function test_reassigned()
    local t = {}
    for i = 1, 3 do
        t[i] = i
    end
    t = { 7, 8, 9 }
    assert_eq(t[1], 7, "reassigned table")
    local sum = 0
    for i = 1, 3 do
        sum = sum + t[i]
    end
    assert_eq(sum, 24, "reassigned table read")
end
test_reassigned()

-- table captured by a closure is excluded but still correct
local function test_captured()
    local t = {}
    local function get(i)
        return t[i]
    end
    for i = 1, 4 do
        t[i] = i * i
    end
    assert_eq(get(4), 16, "captured table read")
    local sum = 0
    for i = 1, 4 do
        sum = sum + t[i]
    end
    assert_eq(sum, 30, "captured table loop sum")
end
test_captured()

-- table parameter to a function: not a single-constructor local
local function fill(t, n)
    for i = 1, n do
        t[i] = i
    end
end
local function test_param_table()
    local t = {}
    t.tag = 0
    fill(t, 6)
    local sum = 0
    for i = 1, 6 do
        sum = sum + t[i]
    end
    assert_eq(sum, 21, "parameter table")
end
test_param_table()

-- do-block containing the loop, and string-key accesses interleaved
local function test_do_block()
    local t = {}
    t.count = 0
    do
        for i = 1, 8 do
            t[i] = i
            t.count = t.count + 1
        end
    end
    local sum = 0
    for i = 1, 8 do
        sum = sum + t[i]
    end
    assert_eq(sum, 36, "do-block loop sum")
    assert_eq(t.count, 8, "do-block field count")
end
test_do_block()

-- A local skipped by a goto is forward-declared but uninitialized; it must not be
-- hoisted (which would dereference a garbage LTable*). clx must still raise the normal
-- "attempt to index a nil value" error rather than crash.
local function test_goto_crossed()
    local ok, err = pcall(function()
        local function inner()
            goto skip
            local t = {}
            ::skip::
            local s = 0
            for i = 1, 3 do
                s = s + t[i]
            end
            return s
        end
        return inner()
    end)
    assert_eq(ok, false, "goto-crossed table raises instead of crashing")
    assert_true(type(err) == "string", "goto-crossed error is a message")

    local ok2 = pcall(function()
        local function nested()
            goto skip
            local t = {}
            ::skip::
            local s = 0
            do
                for i = 1, 3 do
                    s = s + t[i]
                end
            end
            return s
        end
        return nested()
    end)
    assert_eq(ok2, false, "nested goto-crossed table raises instead of crashing")
end
test_goto_crossed()

print_summary("REGRESSION_TABLE_HOIST")
