# FAQ

## What is clx?

clx is a cross-platform ahead-of-time (AOT) compiler for Lua 5.5.

It compiles Lua source code to C++, which is then compiled to native machine code using Clang, GCC, or MSVC.

The result is a standalone executable, static library or object file.

---

## Is clx related to LuaRT?

No.

Although both projects are authored by Samir Tine, clx is a completely separate project with its own compiler, runtime, architecture, and codebase.

LuaRT focuses on Windows application development and deployment.

clx focuses on native compilation of Lua programs.

---

## Why generate C++?

clx uses C++ as an intermediate representation because mature C++ toolchains already provide:

* Excellent optimizers
* Multiple target architectures
* Cross-platform support
* Link-time optimization (LTO)
* Profile-guided optimization (PGO)

This allows clx to benefit from decades of compiler development without maintaining architecture-specific code generators.

---

## Why C++, and why not C (e.g. C99)?

C++ is clx's implementation language in two senses: the runtime library is written in it, and the generated code is C++. Both choices are load-bearing, not incidental.

Exceptions, destructors, and overloads are the mechanisms the runtime's correctness and performance arguments rest on, and every supported compiler already implements the standard completely.

---

## Why not LLVM?

LLVM is an excellent infrastructure, but integrating it would require maintaining a dedicated backend.

clx instead leverages existing C++ compilers as its backend while remaining portable and lightweight.

---

## Why not LuaJIT?

LuaJIT and clx solve different problems.

LuaJIT focuses on runtime JIT compilation.

clx focuses on:

* Ahead-of-time compilation
* Standalone deployment
* Fast startup times
* Predictable performance
* Native toolchain integration

Some workloads may perform better under LuaJIT, while others will benefit from clx's AOT model.

---

## Does clx support Lua 5.5?

Yes, Lua 5.5 compatibility is the project's target.

See [`doc/compatibility.md`](doc/compatibility.md) for detailed support status.

---

## How do load(), loadfile() and dofile() work?

These functions are available when the generated program is compiled with `--dynamic`:

```bash
clx script.lua --dynamic --output script
```

The generated program then links the embedded Lua VM. Source passed to `load`, `loadfile`, or `dofile` executes in that VM and crosses the clx/VM bridge when it calls AOT functions or returns values. Without `--dynamic`—and in `--minimal` builds—the loader functions are not registered.

`string.dump` and `debug` do not depend on `--dynamic` (see the next question). Dynamic code runs in the embedded VM and uses its own standard libraries; see [`doc/dynamic-lua.md`](doc/dynamic-lua.md) for the environment and bridge boundaries.

---

## Does dynamic Lua provide string.dump() and debug?

Yes, both work in every build mode, with one caveat each. `string.dump` of a compiled or C function returns a callable snapshot instead of bytecode — only VM-loaded functions dump real bytecode — and `debug` is present, but the entries that need frame or line tables (`getlocal`, `setlocal`, `getinfo`, `sethook`/`gethook`) raise. Under `--dynamic`, a loaded chunk sees the embedded VM's own `string` and `debug`, so a real `string.dump` and the complete `debug` are reachable there. Consult [Dynamic Lua](doc/dynamic-lua.md) for the boundary details and [Lua 5.5 compatibility](doc/compatibility.md#the-debug-library) for the per-entry status.

---

## Which platforms are supported?

Any platform supported by Clang, GCC, or MSVC.

Examples include:

* Linux
* Windows
* macOS

---

## Which CPU architectures are supported?

Any architecture supported by the underlying C++ compiler, including:

* x86-64
* ARM64
* RISC-V (not tested)

---

## Can I write native modules?

Yes.

clx provides a C++ API for creating native modules that can be linked statically.

See [`doc/modules.md`](doc/modules.md).

---

## Is clx a transpiler?

clx translates Lua to C++ as an intermediate representation.

The generated C++ is then compiled to native machine code.

In practice, clx behaves as an ahead-of-time native compiler.

---

## How small are generated executables?

Executable size depends on:

* Optimization profile
* Linked runtime modules
* Target platform

Typical examples (measured on macOS ARM64 with Apple clang; other platforms vary):

| Program                        | Size    |
| ------------------------------ | ------- |
| Minimal runtime (`--minimal`)  | ~110 KB |
| Default runtime (`--size`)     | ~250 KB |
| Pong + Sokol (`--size`)        | ~500 KB |
| Pong + Sokol (`--fast`)        | ~700 KB |

Actual results depend on platform and compiler.
