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

-- #44 body reproducer: constructor typing must not survive t = z reassignment
local t = { x = 42 }
local z = {}
t = z
assert_eq(t.x, nil, "reassigned table field read is nil")

-- reassignment to another typed table must re-read the new shape
local t2 = { x = 42 }
local w = { x = 7 }
t2 = w
assert_eq(t2.x, 7, "reassigned to another record reads new value")

-- reassignment before any field read
local t3 = { x = 42 }
local u = { y = "str" }
t3 = u
assert_eq(t3.x, nil, "reassigned before read: old field gone")
assert_eq(t3.y, "str", "reassigned before read: new field visible")

-- field still numeric-typed when the table is never reassigned
local t4 = { x = 3, y = 4 }
t4.x = t4.x + 1
assert_eq(t4.x, 4, "un-reassigned record keeps numeric typing")

-- cross-function contamination (issue #44 title / #45)
local function numeric_user(f)
    return f.fields * 2
end

local function table_user(r)
    return r.fields.name
end

local rec = { fields = { name = "ok" } }
assert_eq(table_user(rec), "ok", "table_user before numeric_user")
assert_eq(numeric_user({ fields = 21 }), 42, "numeric_user gets number")
assert_eq(table_user(rec), "ok", "table_user after numeric_user")

print("-----")
print("SUITE :", "numeric field reassign")
print("PASS  :", passed)
print("FAIL  :", failed)
print("-----")
if failed > 0 then
    error("numeric_field_reassign: " .. failed .. " failure(s)")
end
