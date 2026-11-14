-- Source generator for the vendored Lua 5.5.1 testsuite.
-- The runners compile each generated file with clx, so the tests themselves are
-- AOT-compiled: only the chunks a test builds at run time (load/loadfile) go
-- through the embedded VM. The harness half of all.lua (T, showmem, report, the
-- dump/undump dofile wrapper) is reproduced verbatim, and the test body is put in
-- its own vararg function, exactly like the chunk stock Lua would compile.
--
-- usage: harness --list [--extra]
--        harness --emit <test.lua> <out.lua>
--        harness --emit-all <out.lua>

local EXTRA = { "bwcoercion.lua", "heavy.lua" }
-- The string-coercion helper: tests that require it are compiled with it inlined, so the
-- metamethods it installs land on the compiled chunk's string metatable instead of the VM's.
local BWCOERCION = "bwcoercion.lua"
local HARNESS_FILES = { ["all.lua"] = true, ["main.lua"] = true, ["tracegc.lua"] = true }
-- A test's run-time require of a helper module is inlined too (bwcoercion, tracegc), so those
-- modules are AOT-compiled: a required .lua file otherwise loads through the embedded VM, and a VM
-- function cannot be called from inside a clx coroutine. cstack.lua is the exception: its
-- stack-overflow stress needs tracegc's re-arming __gc to run through the VM, because the compiled
-- module finalizing on every allocation makes the whole test crawl.
local VM_TRACEGC_FILES = { ["cstack.lua"] = true }

local function fail(message)
    io.stderr:write(message, "\n")
    os.exit(2)
end

local function arg_list()
    local out = {}
    if type(arg) == "table" then
        local i = 1
        while arg[i] ~= nil do
            out[#out + 1] = arg[i]
            i = i + 1
        end
    end
    return out
end

local function read_file(name)
    local f = io.open(name, "rb")
    if not f then
        return nil
    end
    local data = f:read("a")
    f:close()
    return (data:gsub("\r\n", "\n"):gsub("\r", "\n"))
end

local function read_source()
    local src = read_file("all.lua")
    if not src then
        fail("cannot read all.lua from the current directory (run the harness inside the suite copy)")
    end
    return (src:gsub("^#[^\n]*\n", ""))
end

local function split_lines(src)
    local out = {}
    for line in (src .. "\n"):gmatch("(.-)\n") do
        out[#out + 1] = line
    end
    return out
end

local function is_main_call(line)
    return line:match("^dofile%s*%(%s*['\"]main%.lua['\"]%s*%)%s*$") ~= nil
end

-- Index of the first line that drives main.lua (the stand-alone interpreter test).
local function main_line(lines)
    for i = 1, #lines do
        if is_main_call(lines[i]) then
            return i
        end
    end
    return nil
end

-- The files all.lua drives, in its order, deduplicated.
local function suite_files(lines)
    local seen, out = {}, {}
    for i = 1, #lines do
        local name = lines[i]:match("['\"]([%w_%-%.]+)%.lua['\"]")
        if name and not HARNESS_FILES[name .. ".lua"] and not seen[name] then
            seen[name] = true
            if read_file(name .. ".lua") then
                out[#out + 1] = name .. ".lua"
            end
        end
    end
    return out
end

local function list_tests(extra)
    local out = suite_files(split_lines(read_source()))
    if extra then
        local seen = {}
        for _, n in ipairs(out) do
            seen[n] = true
        end
        for _, n in ipairs(EXTRA) do
            if not seen[n] and read_file(n) then
                out[#out + 1] = n
            end
        end
    end
    return out
end

-- The prelude half of all.lua, up to the line that drives main.lua.
-- The prelude's 'global' declarations are skipped: upstream runs every test in its own
-- chunk, where they do not apply, but here the test body shares the prelude's chunk and a
-- 'global x' declaration makes the rest of that chunk strict about undeclared globals.
local function prelude_lines(lines)
    local stop = main_line(lines)
    if not stop then
        fail("all.lua has no dofile('main.lua') line to cut the harness at")
    end
    local out = {}
    for i = 1, stop - 1 do
        local indent, rest = lines[i]:match("^(%s*)global%s+function%s+(.*)$")
        if indent then
            out[#out + 1] = indent .. "function " .. rest
        elseif not lines[i]:match("^%s*global[%s<]") then
            out[#out + 1] = lines[i]
        end
    end
    return out
end

-- _soft / _port must be set before the prelude reads them.
local function settings_line()
    local out = {}
    if os.getenv("CLX_LUA55_SOFT") == "1" then
        out[#out + 1] = "_soft = true"
    end
    if os.getenv("CLX_LUA55_PORT") == "1" then
        out[#out + 1] = "_port = true"
    end
    return out
end

local function write_lines(path, lines)
    local f, err = io.open(path, "wb")
    if not f then
        fail(err or ("cannot write " .. path))
    end
    f:write(table.concat(lines, "\n"), "\n")
    f:close()
end

local function emit_one(name, path)
    local body = read_file(name)
    if not body then
        fail("missing test file: " .. name)
    end
    local out = settings_line()
    for _, line in ipairs(prelude_lines(split_lines(read_source()))) do
        out[#out + 1] = line
    end
    if body:find("require%s*[\"']tracegc[\"']") and not VM_TRACEGC_FILES[name] then
        local src = read_file("tracegc.lua")
        if not src then
            fail("missing test helper: tracegc.lua")
        end
        out[#out + 1] = "local function _clx_tracegc()"
        for _, line in ipairs(split_lines(src)) do
            if not line:match("^%s*global[%s<]") and not line:match("^%s*global%s+function%s") then
                out[#out + 1] = line
            end
        end
        out[#out + 1] = "end"
        out[#out + 1] = "package.preload['tracegc'] = _clx_tracegc"
    end
    if body:find("require%s*[\"']bwcoercion[\"']") then
        local src = read_file(BWCOERCION)
        if not src then
            fail("missing test helper: " .. BWCOERCION)
        end
        out[#out + 1] = "local function _clx_bwcoercion()"
        for _, line in ipairs(split_lines(src)) do
            if not line:match("^%s*global[%s<]") and not line:match("^%s*global%s+function%s") then
                out[#out + 1] = line
            end
        end
        out[#out + 1] = "end"
        out[#out + 1] = "_clx_bwcoercion()"
        out[#out + 1] = "package.preload['bwcoercion'] = function() return true end"
    end
    out[#out + 1] = "report(" .. string.format("%q", name) .. ")"
    out[#out + 1] = "local function _clx_chunk(...)"
    for _, line in ipairs(split_lines(body)) do
        out[#out + 1] = line
    end
    out[#out + 1] = "end"
    out[#out + 1] = "local _clx_ok, _clx_err = pcall(_clx_chunk)"
    out[#out + 1] = "if not _clx_ok then io.stderr:write(tostring(_clx_err), '\\n'); os.exit(1) end"
    out[#out + 1] = "end"
    write_lines(path, out)
end

local function emit_all(path)
    local lines = split_lines(read_source())
    local stop = main_line(lines)
    local out = settings_line()
    for i = 1, #lines do
        if not (stop and i == stop) then
            out[#out + 1] = lines[i]
        end
    end
    write_lines(path, out)
end

local function main()
    local args = arg_list()
    local mode = args[1]

    if mode == "--list" then
        local extra = false
        for i = 2, #args do
            if args[i] == "--extra" then
                extra = true
            end
        end
        for _, name in ipairs(list_tests(extra)) do
            print(name)
        end
        return
    end

    if mode == "--emit" then
        if not args[2] or not args[3] then
            fail("usage: harness --emit <test.lua> <out.lua>")
        end
        return emit_one(args[2], args[3])
    end

    if mode == "--emit-all" then
        if not args[2] then
            fail("usage: harness --emit-all <out.lua>")
        end
        return emit_all(args[2])
    end

    fail("usage: harness --list [--extra] | --emit <test.lua> <out.lua> | --emit-all <out.lua>")
end

main()
