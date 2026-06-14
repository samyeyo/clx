# Getting started with clx

## Quickstart

### Build clx

```bash
# Clone the repository
git clone https://github.com/yourusername/clx.git
cd clx

# Build (release mode)
./build.sh

# Debug build
./build.sh debug

# Build and install to system
./build.sh install

# Clean build directory
./build.sh clean
```

Alternatively, build manually with CMake:

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Compile your first Lua program

Create a file called `hello.lua`:

```lua
print("Hello, World!")
```

Compile and run:

```bash
./build/clx hello.lua
./hello
```

You should see `Hello, World!` printed.

## Your second program

Let's try something more interesting:

```lua
-- fib.lua
function fib(n)
    if n <= 1 then return n end
    return fib(n - 1) + fib(n - 2)
end

print("Fibonacci(20) = " .. fib(20))
```

Compile it with `--fast` flag for better performances:

```bash
./build/clx --fast fib.lua
./fib
```

It should run faster than Lua 5.5

## Language Features

clx supports most Lua 5.5 features:

### Variables and Types

```lua
-- Numbers
local x = 42
local pi = 3.14159

-- Strings
local greeting = "Hello"
local name = 'World'

-- Booleans
local flag = true

-- Tables
local t = { a = 1, b = 2 }
local arr = { 1, 2, 3 }

-- Functions
local function add(a, b)
    return a + b
end

-- Closures
local function counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end
```

### Control Flow

```lua
-- If/else
if x > 10 then
    print("big")
elseif x > 5 then
    print("medium")
else
    print("small")
end

-- While loop
while x > 0 do
    print(x)
    x = x - 1
end

-- For loop (numeric)
for i = 1, 10 do
    print(i)
end

-- For loop (generic)
for k, v in pairs(t) do
    print(k, v)
end

-- Repeat/until
repeat
    x = x - 1
until x == 0
```

### Functions

```lua
-- Basic function
function greet(name)
    return "Hello, " .. name
end

-- Multiple return values
function divmod(a, b)
    return math.floor(a / b), a % b
end

-- Variadic
function sum(...)
    local total = 0
    for i = 1, select("#", ...) do
        total = total + select(i, ...)
    end
    return total
end

-- Method syntax
local obj = { value = 10 }
function obj:double()
    self.value = self.value * 2
end
```

### Tables and Metatables

```lua
-- Table with methods
local vector = {
    x = 0, y = 0,
    
    add = function(self, other)
        return { x = self.x + other.x, y = self.y + other.y }
    end,
    
    __tostring = function(self)
        return "(" .. self.x .. "," .. self.y .. ")"
    end
}

-- Metatable for operator overloading
setmetatable(vector, {
    __add = function(a, b)
        return a:add(b)
    end
})

local v1 = { x = 1, y = 2 }
local v2 = { x = 3, y = 4 }
local v3 = v1 + v2  -- Uses __add
```

### Coroutines

```lua
-- Producer/consumer with coroutines
local function producer(max)
    for i = 1, max do
        coroutine.yield(i)
    end
end

local function consumer()
    local co = coroutine.create(producer)
    while true do
        local status, value = coroutine.resume(co)
        if not status or value == nil then break end
        print("Received: " .. value)
    end
end

consumer()
```

### String module

```lua
-- Basic operations
local s = "Hello, World!"
print(string.len(s))          -- 13
print(string.sub(s, 1, 5))    -- Hello
print(string.upper(s))        -- HELLO, WORLD!
print(string.lower(s))        -- hello, world!
print(string.reverse(s))      -- !dlroW ,olleH

-- Character conversion
print(string.byte("A"))       -- 65
print(string.char(65, 66, 67)) -- ABC

-- Repetition
print(string.rep("ab", 3))    -- ababab
print(string.rep("x", 5, "-")) -- x-x-x-x-x

-- Format
print(string.format("Pi: %.2f", 3.14159))  -- Pi: 3.14
print(string.format("%d + %d = %d", 1, 2, 3))  -- 1 + 2 = 3

-- Pattern matching
local start, finish = string.find("hello world", "world")
print(start, finish)  -- 7 11

local match = string.match("hello world", "(%a+)")
print(match)  -- hello

for word in string.gmatch("hello world from lua", "%a+") do
    print(word)
end

local result, count = string.gsub("hello world", "world", "lua")
print(result, count)  -- hello lua, 1
```

### Bitwise operations

```lua
-- Bitwise AND, OR, XOR
print(0xFF & 0x0F)    -- 15
print(0xF0 | 0x0F)    -- 255
print(0xFF ~ 0xF0)    -- 15

-- Bitwise shifts
print(1 << 8)         -- 256
print(256 >> 4)       -- 16

-- Bitwise NOT
print(~0)             -- -1
```

## Performance tips

### Use local variables

```lua
-- Good: local variables are faster
local function compute()
    local result = 0
    for i = 1, 1000 do
        local temp = i * 2
        result = result + temp
    end
    return result
end
```

### Prefer numeric for loops

```lua
-- Good: numeric for loops can be optimized
for i = 1, 1000000 do
    -- body
end
```

### Avoid mixing types

```lua
-- Slower: mixed type operations
local x = 1 + "2"  -- Requires runtime type check

-- Faster: same types
local x = 1 + 2    -- Direct arithmetic
```

## Common issues

### Debugging Compilation Errors

If you get a C++ compilation error, you can see the generated code:

```bash
clx script.lua --cpp
# This creates script.cpp
# You can examine it to see what's being generated
```

### Understanding runtime errors

Runtime errors show the Lua line where the error occurred:

```
Error: script.lua:10: attempt to perform arithmetic on a number value
```

The format is `filename:line: message`

## Next Steps

- Read the [Architecture](./architecture.md) document
- Learn about [Optimizations](./optimizations.md)
- Explore the [Runtime](./runtime.md) implementation
- Check out the [CLI](./cli.md) options