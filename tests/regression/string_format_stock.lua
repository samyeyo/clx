local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        print("[OK]   " .. name)
    else
        print("[FAIL] " .. name .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual))
        failed = failed + 1
    end
end

local function assert_err(fn, needle, name)
    local ok, e = pcall(fn)
    if ok then
        print("[FAIL] " .. name .. ": expected error, got " .. tostring(e))
        failed = failed + 1
    elseif not tostring(e):find(needle, 1, true) then
        print("[FAIL] " .. name .. ": error " .. tostring(e) .. " missing '" .. needle .. "'")
        failed = failed + 1
    else
        print("[OK]   " .. name)
    end
end

assert_eq(("%d"):format(2 ^ 53), "9007199254740992", "d 2^53")
assert_eq(("%d"):format(2 ^ 53 - 1), "9007199254740991", "d 2^53-1")
assert_eq(("%i"):format(2 ^ 53), "9007199254740992", "i 2^53")
assert_eq(("%d"):format(1e15), "1000000000000000", "d 1e15")
assert_eq(("%d"):format(-2 ^ 53), "-9007199254740992", "d -2^53")
assert_eq(("%x"):format(2 ^ 53), "20000000000000", "x 2^53")
assert_eq(("%X"):format(2 ^ 53), "20000000000000", "X 2^53")
assert_eq(("%o"):format(64.0), "100", "o 64.0")
assert_eq(("%u"):format(-1), "18446744073709551615", "u -1")
assert_eq(("%d"):format(math.maxinteger), "9223372036854775807", "d maxinteger")
assert_eq(("%d"):format(math.mininteger), "-9223372036854775808", "d mininteger")
assert_eq(("%d"):format("12"), "12", "d numeric string")

assert_err(function() string.format("%d", 3.5) end, "no integer representation", "d 3.5 errors")
assert_err(function() string.format("%d", 1e20) end, "no integer representation", "d 1e20 errors")
assert_err(function() string.format("%c", 65.5) end, "no integer representation", "c 65.5 errors")
assert_err(function() string.format("%d") end, "no value", "d no value errors")

assert_err(function() string.format("%hd", 1) end, "invalid conversion", "hd rejected")
assert_err(function() string.format("%ld", 1) end, "invalid conversion", "ld rejected")
assert_err(function() string.format("%lld", 1) end, "invalid conversion", "lld rejected")
assert_err(function() string.format("%#d", 5) end, "invalid conversion specification", "#d rejected")
assert_err(function() string.format("%+u", 5) end, "invalid conversion specification", "+u rejected")
assert_err(function() string.format("%12345678901d", 1) end, "invalid conversion specification", "wide rejected")
assert_err(function() string.format("%*d", 5, 1) end, "invalid conversion", "*d rejected")
assert_err(function() string.format("%y", 1) end, "invalid conversion", "y rejected")

assert_eq(("%5d|"):format(42), "   42|", "d width")
assert_eq(("%-5d|"):format(42), "42   |", "d left")
assert_eq(("%05d"):format(42), "00042", "d zero pad")
assert_eq(("%+d"):format(42), "+42", "d plus")
assert_eq(("%08.3f"):format(3.14159), "0003.142", "f width")

-- %a delegates to the C library, exactly like stock Lua, so its shape is platform-defined:
-- glibc/musl trim the fraction to the shortest exact form, Windows' UCRT always prints all
-- 13 fractional hex digits. Expectations are written in the trimmed form and expanded here.
local CRT_PADS_HEXFLOAT = #(("%a"):format(1.0)) > #"0x1p+0"
local function hexfloat(trimmed)
    if not CRT_PADS_HEXFLOAT then
        return trimmed
    end
    local sign, lead, frac, expo = trimmed:match("^(%-?0[xX])(%x)(%.?%x*)([pP][-+]%d+)$")
    if frac == "" then
        frac = "."
    end
    return sign .. lead .. frac .. string.rep("0", 13 - (#frac - 1)) .. expo
end

assert_eq(("%a"):format(1.0), hexfloat("0x1p+0"), "a 1.0")
assert_eq(("%a"):format(0.0), hexfloat("0x0p+0"), "a 0.0")
assert_eq(("%a"):format(-0.0), hexfloat("-0x0p+0"), "a -0.0")
assert_eq(("%a"):format(1.5), hexfloat("0x1.8p+0"), "a 1.5")
assert_eq(("%a"):format(1e10), hexfloat("0x1.2a05f2p+33"), "a 1e10")
assert_eq(("%a"):format(1 / 0), "inf", "a inf")
assert_eq(("%a"):format(-(1 / 0)), "-inf", "a -inf")
assert_eq(("%A"):format(1.5), hexfloat("0X1.8P+0"), "A 1.5")
assert_eq(("%.2a"):format(1.0), "0x1.00p+0", "a precision")

assert_eq(("%s"):format(10), "10", "s number")
assert_eq(("%s"):format(true), "true", "s boolean")
assert_eq(("%s"):format(nil), "nil", "s nil")
assert_eq(("%5.2s"):format("hello"), "   he", "s width precision")
assert_eq(("%-8s|"):format("x"), "x       |", "s left")
local z = ("%s"):format("a\0b")
assert_eq(#z, 3, "s embedded zero length")
assert_eq(z:byte(2), 0, "s embedded zero byte")

assert_eq(("%q"):format("a\tb\nc"), "\"a\\9b\\\nc\"", "q escapes")
assert_err(function() string.format("%10q", "x") end, "cannot have modifiers", "q modifiers rejected")

assert_err(function() string.sub("hello", 1.5) end, "no integer representation", "sub 1.5 errors")
assert_err(function() string.byte("x", 1e20) end, "no integer representation", "byte 1e20 errors")
assert_eq(string.sub("hello", 1.0, 3.0), "hel", "sub float ints")
assert_eq(string.rep("ab", 3.0), "ababab", "rep float int")

print("-----")
print("string_format: " .. (failed == 0 and "PASS" or "FAIL") .. " (" .. failed .. " failures)")
if failed > 0 then
    os.exit(1)
end
