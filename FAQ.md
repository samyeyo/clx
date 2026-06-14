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

See `docs/compatibility.md` for detailed support status.

---

## Why are load(), loadfile(), string.dump() and dofile() unsupported?

These functions require runtime code compilation.

Since clx follows an AOT model and does not embed a Lua interpreter, they are not supported.

---

## Why is the debug module unsupported?

The `debug` module depends heavily on interpreter internals and runtime introspection facilities that do not exist in compiled native executables.

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

clx provides a C++ API for creating native modules that can be linked statically or dynamically.

See `docs/modules.md`.

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

Typical examples:

| Program                        | Size    |
| ------------------------------ | ------- |
| Minimal runtime (`--minimal`)  | ~80 KB  |
| Default runtime                | ~170 KB |
| Pong + Sokol (`--size`)        | ~500 KB |
| Pong + Sokol (`--fast`)        | ~700 KB |

Actual results depend on platform and compiler.
