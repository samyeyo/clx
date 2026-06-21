# Lua 5.5 Compatibility

clx targets Lua 5.5 compatibility.

The following table summarizes the current implementation status, but real life tests are still needed

## Core Language

| Feature              | Status |
| -------------------- | ------ |
| Variables            | ✅      |
| Arithmetic operators | ✅      |
| Logical operators    | ✅      |
| Comparisons          | ✅      |
| Functions            | ✅      |
| Closures             | ✅      |
| _ENV                 | ✅      |
| Varargs              | ✅      |
| Multiple returns     | ✅      |
| Local variables      | ✅      |
| Global variables     | ✅      |

## Control Flow

| Feature            | Status |
| ------------------ | ------ |
| if / elseif / else | ✅      |
| while              | ✅      |
| repeat / until     | ✅      |
| numeric for        | ✅      |
| generic for        | ✅      |
| break              | ✅      |
| goto               | ✅      |
| labels             | ✅      |

## Tables

| Feature            | Status |
| ------------------ | ------ |
| Table constructors | ✅      |
| Array part         | ✅      |
| Hash part          | ✅      |
| Mixed tables       | ✅      |
| Table iteration    | ✅      |

## Metatables

| Feature    | Status |
| ---------- | ------ |
| __index    | ✅      |
| __newindex | ✅      |
| __add      | ✅      |
| __sub      | ✅      |
| __mul      | ✅      |
| __div      | ✅      |
| __mod      | ✅      |
| __pow      | ✅      |
| __unm      | ✅      |
| __len      | ✅      |
| __concat   | ✅      |
| __eq       | ✅      |
| __lt       | ✅      |
| __le       | ✅      |
| __call     | ✅      |
| __tostring | ✅      |
| __ipairs   | ✅      |
| __pairs    | ✅      |

## Coroutines

| Feature          | Status |
| ---------------- | ------ |
| coroutine.create | ✅      |
| coroutine.resume | ✅      |
| coroutine.yield  | ✅      |
| coroutine.status | ✅      |
| coroutine.wrap   | ✅      |
| coroutine.close  | ✅      |

## Modules

| Feature         | Status |
| --------------- | ------ |
| require         | ✅      |
| package.path    | ✅      |
| Native modules  | ✅      |
| Modules linking | ✅      |

## Standard Libraries

| Library   | Status |
| --------- | ------ |
| base      | ✅      |
| math      | ✅      |
| string    | ✅      |
| table     | ✅      |
| coroutine | ✅      |
| io        | ✅      |
| os        | ✅      |
| utf8      | ✅      |
| package   | ✅      |
| debug     | ❌      |

## Unsupported Features

The following features are intentionally unsupported due to the AOT compilation model:

| Feature       | Reason                           |
| ------------- | -------------------------------- |
| load()        | Lua code loading                 |
| loadfile()    | Lua code loading from file       |
| dofile()      | Lua code running from file       |
| string.dump() | Lua code compilation             |
| debug library | Lua runtime introspection        |


## Notes

clx is under active development.

Additional Lua 5.5 compatibility work, regression testing, and edge-case handling are needed.
