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
| debug     | AOT: ❌ / VM: ✅ with `--dynamic` |

## Conditional and Unsupported Features

The following loading functions are available only when the generated program is compiled with `--dynamic`. They execute source in the embedded Lua VM and cross the clx/VM bridge; they are not available in ordinary AOT or `--minimal` builds.

| Feature    | Status | Notes |
| ---------- | ------ | ----- |
| `load()`   | ⚠️     | Requires `--dynamic`; the current bridge accepts source strings, not reader functions. |
| `loadfile()` | ⚠️   | Requires `--dynamic`; compiles a file in the embedded VM. |
| `dofile()` | ⚠️    | Requires `--dynamic`; loads and executes a file through the bridge. |

The following features are not provided by the clx AOT runtime directly. The embedded VM behavior under `--dynamic` is noted explicitly:

| Feature       | Reason                           |
| ------------- | -------------------------------- |
| `string.dump()` | Not provided by the clx AOT runtime; available inside the embedded VM with `--dynamic` |
| debug library | Not provided as a clx AOT global; dynamic VM behavior is documented separately |


## Notes

clx is under active development.

Additional Lua 5.5 compatibility work, regression testing, and edge-case handling are needed.
