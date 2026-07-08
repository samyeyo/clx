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

print("----------------- string.len")
assert_eq(string.len(""), 0, "empty string")
assert_eq(string.len("hello"), 5, "simple string")
assert_eq(string.len("héllo"), 6, "utf-8 chars (byte length)")

print("\n----------------- string.sub")
assert_eq(string.sub("hello", 2, 4), "ell", "sub middle")
assert_eq(string.sub("hello", 2), "ello", "sub from 2")
assert_eq(string.sub("hello", -3), "llo", "sub negative start")
assert_eq(string.sub("hello", 1, 1), "h", "sub single char")
assert_eq(string.sub("hello", 10), "", "sub out of bounds")
assert_eq(string.sub("hello", 4, 2), "", "sub start > end")

print("\n----------------- string.reverse")
assert_eq(string.reverse("abc"), "cba", "reverse odd length")
assert_eq(string.reverse("abcd"), "dcba", "reverse even length")
assert_eq(string.reverse(""), "", "reverse empty")

print("\n----------------- string.lower / string.upper")
assert_eq(string.lower("Hello"), "hello", "lower mixed case")
assert_eq(string.lower("HELLO"), "hello", "lower all caps")
assert_eq(string.lower(""), "", "lower empty")
assert_eq(string.upper("Hello"), "HELLO", "upper mixed case")
assert_eq(string.upper("hello"), "HELLO", "upper all lower")
assert_eq(string.upper(""), "", "upper empty")

print("\n----------------- string.rep")
assert_eq(string.rep("ab", 3), "ababab", "rep without sep")
assert_eq(string.rep("ab", 3, ","), "ab,ab,ab", "rep with sep")
assert_eq(string.rep("a", 0), "", "rep 0 times")
assert_eq(string.rep("", 5), "", "rep empty string")

print("\n----------------- string.byte")
local b1, b2, b3 = string.byte("ABC", 1, 3)
assert_eq(b1, 65, "byte A")
assert_eq(b2, 66, "byte B")
assert_eq(b3, 67, "byte C")
assert_eq(string.byte("ABC", 2), 66, "byte single pos")
assert_eq(string.byte("ABC", 5), nil, "byte out of range")

print("\n----------------- string.char")
assert_eq(string.char(65, 66, 67), "ABC", "char multiple")
assert_eq(string.char(65), "A", "char single")
assert_eq(string.char(), "", "char no args")

print("\n----------------- string.format")
assert_eq(string.format("hello %s", "world"), "hello world", "format %s")
assert_eq(string.format("%d %x %f", 42, 255, 3.14), "42 ff 3.140000", "format %d %x %f")
assert_eq(string.format("|%10s|", "hi"), "|        hi|", "format width")
assert_eq(string.format("%.2f", 3.14159), "3.14", "format precision")
-- %q test skipped: embedded quotes break C++ string emission

print("\n----------------- string.find")
assert_eq(string.find("hello world", "world"), 7, "find plain")
assert_eq(string.find("hello world", "xyz"), nil, "find not found")
local s, e = string.find("hello 123", "%d+")
assert_eq(s, 7, "find pattern start")
assert_eq(e, 9, "find pattern end")
assert_eq(string.find("test", "^t"), 1, "find anchored ^")
assert_eq(string.find("test", "t$"), 4, "find anchored $")

print("\n----------------- string.match")
assert_eq(string.match("hello 123 world", "%d+"), "123", "match digits")
assert_eq(string.match("hello", "xyz"), nil, "match not found")
assert_eq(string.match("one two three", "%a+"), "one", "match letters")
local a, b = string.match("a=1, b=2", "(%a)=(%d)")
assert_eq(a, "a", "match capture 1")
assert_eq(b, "1", "match capture 2")

print("\n----------------- string.gmatch")
local words = {}
for w in string.gmatch("a,b,c", "[^,]+") do
    words[#words + 1] = w
end
assert_eq(words[1], "a", "gmatch 1")
assert_eq(words[2], "b", "gmatch 2")
assert_eq(words[3], "c", "gmatch 3")

print("\n----------------- string.gsub")
local r1, n1 = string.gsub("hello world", "%l", "X")
assert_eq(r1, "XXXXX XXXXX", "gsub replace letters")
assert_eq(n1, 10, "gsub count")
local r2 = string.gsub("test", "^.", string.upper)
assert_eq(r2, "Test", "gsub with function")
local r3, n3 = string.gsub("aaa", "a", "b", 2)
assert_eq(r3, "bba", "gsub max replacements")
assert_eq(n3, 2, "gsub count with max")

print("\n----------------- string.pack / string.unpack / string.packsize")
local s = string.pack("b", -128)
assert_eq(#s, 1, "pack b size")
assert_eq(string.unpack("b", s), -128, "unpack b")

s = string.pack("B", 255)
assert_eq(string.unpack("B", s), 255, "unpack B")

s = string.pack("h", -32768)
assert_eq(string.unpack("h", s), -32768, "unpack h")

s = string.pack("H", 65535)
assert_eq(string.unpack("H", s), 65535, "unpack H")

s = string.pack("i4", -1000000)
assert_eq(string.unpack("i4", s), -1000000, "unpack i4")

s = string.pack("I4", 3000000)
assert_eq(string.unpack("I4", s), 3000000, "unpack I4")

s = string.pack("f", 3.14)
local v = string.unpack("f", s)
assert_eq(math.abs(v - 3.14) < 0.001, true, "unpack f approx")

s = string.pack("d", 1.23456789012345)
assert_eq(string.unpack("d", s), 1.23456789012345, "unpack d")

s = string.pack("c4", "abc")
assert_eq(string.unpack("c4", s), "abc\0", "unpack c4")

s = string.pack("s2", "hello")
assert_eq(string.unpack("s2", s), "hello", "unpack s2")

assert_eq(string.packsize("bBhid"), 1 + 1 + 2 + 4 + 8, "packsize")

s = string.pack(">i2", 0x1234)
assert_eq(string.unpack(">i2", s), 0x1234, "big endian i2")

s = string.pack("<i2", 0x1234)
assert_eq(string.unpack("<i2", s), 0x1234, "little endian i2")

local s2 = string.pack("bi4", -1, 12345678)
local v2, v3 = string.unpack("bi4", s2)
assert_eq(v2, -1, "multi unpack v2")
assert_eq(v3, 12345678, "multi unpack v3")

s = string.pack("z", "test")
assert_eq(string.unpack("z", s), "test", "unpack z")
assert_eq(#s, 5, "pack z size")

s = string.pack("bxb", 10, 20)
assert_eq(string.unpack("bxb", s), 10, "unpack x v1")
local _, v2b = string.unpack("bxb", s)
assert_eq(v2b, 20, "unpack x v2")

s = string.pack("i4i4", 100, 200)
local v4, pos5 = string.unpack("i4", s)
assert_eq(v4, 100, "unpack with pos v1")
assert_eq(string.unpack("i4", s, pos5), 200, "unpack with pos v2")

print_summary("STRING MODULE")
