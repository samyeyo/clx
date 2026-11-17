-- demo.lua — using the fs module from a Lua script
local fs = require "fs"

fs.mkdir("demo_dir/sub")
fs.write("demo_dir/hello.txt", "Hello from clx!\n")
fs.append("demo_dir/hello.txt", "A second line.\n")

print(fs.read("demo_dir/hello.txt"))

-- Functions that can fail return nil plus a message:
local data, err = fs.read("no_such_file.txt")
if not data then print("could not read:", err) end

-- A table of names, and a table of details:
for _, name in ipairs(fs.list("demo_dir")) do print("-", name) end
local info = fs.info("demo_dir/hello.txt")
print(info.name, info.kind, info.size .. " bytes")

-- A callback:
fs.each("demo_dir", function(name, kind) print(kind, name) end)

-- Path objects:
local p = fs.path("demo_dir") / "hello.txt"
print(p, p:filename(), p:extension(), p:exists())

fs.remove("demo_dir", true)

print(p, type(p))
