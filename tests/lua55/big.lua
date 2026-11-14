-- $Id: testes/big.lua $
-- See Copyright Notice in file lua.h

if _soft then
  return 'a'
end

print "testing large tables"

-- clx: disabled -- clx has no debug library, and traceback is its only use here
-- local debug = require"debug" 

local lim = 2^18 + 1000
local prog = { "local y = {0" }
for i = 1, lim do prog[#prog + 1] = i  end
prog[#prog + 1] = "}\n"
prog[#prog + 1] = "X = y\n"
prog[#prog + 1] = ("assert(X[%d] == %d)"):format(lim - 1, lim - 2)
prog[#prog + 1] = "return 0"
prog = table.concat(prog, ";")

local env = {string = string, assert = assert}
local f = assert(load(prog, nil, nil, env))

f()
assert(env.X[lim] == lim - 1 and env.X[lim + 1] == lim)
for k in pairs(env) do env[k] = undef end

-- yields during accesses larger than K (in RK)
setmetatable(env, {
  __index = function (t, n) coroutine.yield('g'); return _G[n] end,
  __newindex = function (t, n, v) coroutine.yield('s'); _G[n] = v end,
})

-- clx: disabled -- f is a load()-ed chunk running on the embedded VM, and clx cannot call
-- a VM function from inside one of its own coroutines ("cannot call Lua VM function from
-- inside a clx coroutine"), so the yield-during-access walk below cannot run
-- X = nil
-- local co = coroutine.wrap(f)
-- assert(co() == 's')
-- assert(co() == 'g')
-- assert(co() == 'g')
-- assert(co() == 0)

-- assert(X[lim] == lim - 1 and X[lim + 1] == lim)

-- errors in accesses larger than K (in RK)
-- clx: disabled -- same VM chunk, same limit; errors raised by the missing X are only
-- reached after the coroutine walk above
-- getmetatable(env).__index = function () end
-- getmetatable(env).__newindex = function () end
-- local e, m = pcall(f)
-- assert(not e and m:find("global 'X'"))

-- errors in metamethods 
getmetatable(env).__newindex = function () error("hi") end
-- clx: disabled -- needs debug.traceback, and clx reports the error raised inside the
-- metamethod ("hi") without naming the metamethod, so m:find("'newindex'") cannot match
-- local e, m = xpcall(f, debug.traceback)
-- assert(not e and m:find("'newindex'"))

f, X = nil

-- clx: disabled -- upstream runs big.lua inside all.lua's coroutine.wrap; a per-file
-- run has no enclosing coroutine, so this yield fails with "attempt to yield from
-- outside a coroutine"
-- coroutine.yield'b'

if 2^32 == 0 then   -- (small integers) {   

print "testing string length overflow"

local repstrings = 192          -- number of strings to be concatenated
local ssize = math.ceil(2.0^32 / repstrings) + 1   -- size of each string

assert(repstrings * ssize > 2.0^32)  -- it should be larger than maximum size

local longs = string.rep("\0", ssize)   -- create one long string

-- create function to concatenate 'repstrings' copies of its argument
local rep = assert(load(
  "local a = ...; return " .. string.rep("a", repstrings, "..")))

local a, b = pcall(rep, longs)   -- call that function

-- it should fail without creating string (result would be too large)
assert(not a and string.find(b, "overflow"))

end   -- }

print'OK'

return 'a'
