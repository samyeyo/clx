-- Loads and executes a Lua script via load().
-- Usage: run_load_shim <script.lua>
--   Prepends a package.path extension so that require() inside the
--   loaded chunk can find sibling .lua files (e.g. canada.lua requires
--   dkjson).  The preamble runs inside the Lua VM, not in AOT code,
--   so package is already fully initialized by the Lua VM.
local script_path = arg[1]
if not script_path then
    io.stderr:write("Usage: run_load_shim <script.lua>\n")
    os.exit(1)
end

-- Derive the script's directory for the preamble below, normalizing
-- backslashes to forward slashes so it is safe inside a Lua string literal.
local script_dir = script_path:match("^(.*)[/\\]") or "./"
script_dir = script_dir:gsub("\\", "/")

local f = io.open(script_path, "rb")
if not f then
    io.stderr:write("Error: cannot open ", script_path, "\n")
    os.exit(1)
end
local content = f:read("*a")
f:close()

if not content or content == "" then
    io.stderr:write("Error: empty file ", script_path, "\n")
    os.exit(1)
end

-- Prepend a package.path extension so require() in the loaded chunk
-- (which runs inside the VM) can find sibling modules.  The directory is
-- baked in as a literal so package.path gets the real path at runtime.
local preamble = 'package.path = package.path .. ";" .. "' .. script_dir .. '?.lua"\n'
local load_content = preamble .. content

local chunk, err = load(load_content, script_path, "t")
if not chunk then
    io.stderr:write("load error: ", err, "\n")
    os.exit(1)
end

local ok, run_err = pcall(chunk)
if not ok then
    io.stderr:write("runtime error: ", run_err, "\n")
    os.exit(1)
end