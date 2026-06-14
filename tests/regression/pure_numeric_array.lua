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

print("----------------- Pure numeric array with table-typed values")

-- Bug: arr[i] = tbl_var where tbl_var is a table-typed identifier
-- caused arr to be inferred as vector<double>, then .as_number() gave 0,
-- later as_pointer() segfaulted.

local function test_array_of_tables()
    local rows = {}
    for i = 1, 3 do
        rows[i] = { id = i, name = "row" .. i }
    end
    assert_eq(rows[1].id, 1, "table elem [1].id")
    assert_eq(rows[2].name, "row2", "table elem [2].name")
    assert_eq(rows[3].id, 3, "table elem [3].id")
end
test_array_of_tables()

-- Array of arrays
local function test_nested_arrays()
    local grid = {}
    for y = 1, 3 do
        local row = {}
        for x = 1, 3 do
            row[x] = (y - 1) * 3 + x
        end
        grid[y] = row
    end
    assert_eq(grid[1][1], 1, "grid[1][1]")
    assert_eq(grid[2][2], 5, "grid[2][2]")
    assert_eq(grid[3][3], 9, "grid[3][3]")
end
test_nested_arrays()

-- Mix of numbers and tables in same array (disqualifies pure-numeric)
local function test_mixed_array()
    local t = {}
    t[1] = 42
    t[2] = { val = "hello" }
    t[3] = 99
    assert_eq(t[1], 42, "mixed array [1] number")
    assert_eq(t[2].val, "hello", "mixed array [2] table value")
    assert_eq(t[3], 99, "mixed array [3] number")
end
test_mixed_array()

-- Table elements stored and returned from function
local function make_elem(id)
    return { id = id, active = true }
end

local function test_table_elements_from_func()
    local items = {}
    for i = 1, 3 do
        items[i] = make_elem(i)
    end
    for i = 1, 3 do
        assert_true(items[i].active, "table elem " .. i .. " active flag")
        assert_eq(items[i].id, i, "table elem " .. i .. " id")
    end
end
test_table_elements_from_func()

print_summary("REGRESSION_PURE_NUMERIC_ARRAY")
