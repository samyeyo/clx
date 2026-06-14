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

local function assert_nil(val, name)
    if val == nil then
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

print("----------------- utf8.charpattern")
assert_eq(type(utf8.charpattern), "string", "charpattern exists")
assert_true(#utf8.charpattern > 0, "charpattern non-empty")

print("\n----------------- utf8.char / codepoint round-trip")
assert_eq(utf8.codepoint(utf8.char(65)), 65, "round-trip ASCII")
assert_eq(utf8.codepoint(utf8.char(0x00E9)), 0x00E9, "round-trip U+00E9")
assert_eq(utf8.codepoint(utf8.char(0x4E16)), 0x4E16, "round-trip U+4E16")
assert_eq(utf8.codepoint(utf8.char(0x1F600)), 0x1F600, "round-trip U+1F600")

print("\n----------------- utf8.char edge cases")
assert_eq(utf8.char(), "", "char no args")
local multi2 = utf8.char(65, 66, 67)
assert_eq(utf8.codepoint(multi2, 1), 65, "multi char [1]")
assert_eq(utf8.codepoint(multi2, 2), 66, "multi char [2]")
assert_eq(utf8.codepoint(multi2, 3), 67, "multi char [3]")

print("\n----------------- utf8.codepoint with ranges")
local s = utf8.char(65, 0x00E9, 0x4E16, 0x1F600)
assert_eq(utf8.codepoint(s, 1), 65, "codepoint range [1]")
assert_eq(utf8.codepoint(s, 2), 0x00E9, "codepoint range [2]")
assert_eq(utf8.codepoint(s, 4), 0x4E16, "codepoint range [3]")
assert_eq(utf8.codepoint(s, 7), 0x1F600, "codepoint range [4]")
local a = utf8.codepoint(s, 2)
local b = utf8.codepoint(s, 4)
assert_eq(a, 0x00E9, "codepoint range [2]")
assert_eq(b, 0x4E16, "codepoint range [3]")

print("\n----------------- utf8.len")
assert_eq(utf8.len(""), 0, "len empty")
assert_eq(utf8.len("ABC"), 3, "len ASCII")
local s2 = utf8.char(0x00E9, 0x4E16, 0x1F600)
assert_eq(utf8.len(s2), 3, "len 3 multi-byte chars")
local s3 = "A" .. utf8.char(0x00E9) .. "B"
assert_eq(utf8.len(s3), 3, "len mixed")

print("\n----------------- utf8.offset")
local s4 = "A" .. utf8.char(0x00E9) .. "B" .. utf8.char(0x4E16)
assert_eq(utf8.offset(s4, 1), 1, "offset 1 -> byte 1")
assert_eq(utf8.offset(s4, 2), 2, "offset 2 -> byte 2")
assert_eq(utf8.offset(s4, 3), 4, "offset 3 -> byte 4")
assert_eq(utf8.offset(s4, 4), 5, "offset 4 -> byte 5")
assert_nil(utf8.offset(s4, 10), "offset beyond length -> nil")

print("\n----------------- utf8.codes")
local codes_out = {}
local s5 = "A" .. utf8.char(0x00E9) .. "B"
for pos, cp in utf8.codes(s5) do
    codes_out[#codes_out + 1] = {pos, cp}
end
assert_eq(#codes_out, 3, "codes: 3 codepoints")
assert_eq(codes_out[1][1], 1, "codes: first pos=1")
assert_eq(codes_out[1][2], 65, "codes: first cp=65")
assert_eq(codes_out[2][1], 2, "codes: second pos=2")
assert_eq(codes_out[2][2], 0x00E9, "codes: second cp=233")
assert_eq(codes_out[3][1], 4, "codes: third pos=4")
assert_eq(codes_out[3][2], 66, "codes: third cp=66")

print_summary("UTF8 MODULE")
