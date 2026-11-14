# clx Command-Line Interface

The `clx` command turns Lua files into native programs.

## Usage

```bash
clx [options] <file.lua> [<compiler-options>]
```

Any option that clx doesn't recognize is passed straight through to the C++
compiler underneath (see [Pass-through options](#pass-through-options)).

## The essentials

### Choose what to build

| Option | What it does |
|--------|--------------|
| `--executable` | Build a runnable program (the default) |
| `--object` | Build an object file (`.o`/`.obj`) you can link yourself |
| `--static` | Build a static library (`.a`/`.lib`) |

### Name your output

| Option | What it does |
|--------|--------------|
| `--output <name>` | Set the output file's name (`-o` works as a short alias) |
| `--cxx` | Print the C++ compiler dialect and path clx drives, then exit |

### Control the build

| Option | What it does |
|--------|--------------|
| `--debug` | Keep debugging information so you can set breakpoints in the Lua source |
| `--size` | Optimize for a small binary (default) |
| `--fast` | Optimize for speed instead of size |
| `--cpp` | Write out the generated C++ files without compiling (for debugging) |
| `--minimal` | Link base + package only; add std modules with `--modules string,io,...` (drops string, table, math, io, os, utf8, coroutine, debug) |
| `--dynamic` | Enable `load`, `loadfile`, and `dofile` so code can run at runtime (requires the separate `libclx_lua.a` bridge archive, linked automatically) |
| `--modules <list>` | Link std modules (string, table, math, io, os, utf8, coroutine, debug) or prebuilt native modules (comma-separated). Archives are searched in the current directory, then clx's `lib/clx` install locations, then any `-L<dir>` flags you pass |

### Help

| Option | What it does |
|--------|--------------|
| `--help` | Show the help message |
| `--version` | Print the version and copyright |

## Pass-through options

Anything starting with `-` that clx doesn't recognize is handed to the C++
compiler. This is useful for targeting a specific CPU or tuning things yourself.
clx's default optimization flags are *prepended* before your flags (so a user
`-O` flag wins for the optimization *level*, since it comes last — but the other
defaults such as inlining policy, visibility, DCE, and strip remain). The only
default a user flag suppresses is the baked `CLX_ARCH` flag, when the user
supplies their own `-march=`/`-mcpu=`/`-msse*`/`-mavx*`/`/arch:`.

```bash
# Optimize for speed (user flag comes after clx's defaults)
clx file.lua -O2

# Target a specific CPU (also suppresses the baked CLX_ARCH default)
clx file.lua -march=native

# Combine several options
clx file.lua -O2 -march=native
```

## Common examples

### Compile and run

```bash
clx script.lua
./script            # the output is named after script.lua by default
```

### Give the output a different name

```bash
clx script.lua --output myprogram
./myprogram
```

### Build a debuggable program

```bash
clx script.lua --debug
```

With `--debug` you can step through your Lua source line by line in a debugger.

### Write out the generated C++ (no compile)

```bash
clx script.lua --cpp
# Creates script.cpp in the current directory. Useful when debugging clx itself.
```

### Build an object file

```bash
clx script.lua --object
# Produces script.o (or script.obj on Windows)
```

### Build a static library

```bash
clx script.lua --static
# Produces script.a (or script.lib on Windows)
```

## Choosing speed vs. size

| Goal | Use | Notes |
|------|-----|-------|
| Smallest possible binary | `--size` (default) | Best for scripts and installers |
| Fastest execution | `--fast` | Best for heavy compute |

In practice, most ordinary programs won't notice a big difference between the
two. Choose `--fast` when your program is dominated by math or other
computations, and `--size` when binary size matters more.

## Environment variables

| Variable | What it means |
|----------|---------------|
| `CLX_CXX` | C++ compiler used to build your programs (falls back to the compiler that built clx). The executable name picks the flag style: `cl` and `clang-cl` get MSVC-style flags, anything else gets GCC-style flags. |
| `CLX_ARCH` | Target CPU baked into clx's runtime libs and injected into every program clx compiles (including `--debug`). Values: `sse2`, `avx`, `avx2`, `native` on x86; `native`, `generic` on ARM. Opting into AVX2 or newer enables faster SIMD paths in the runtime. An explicit user `-march=`/`-mcpu=` flag takes precedence and the baked default is not added. |
| `CLX_GC_MODE` | Collector for compiled programs: `generational` (default) or `incremental` (the legacy collector). |
| `CLX_GC_MINOR_KB` | Pins the minor-collection threshold in kilobytes. Without it the threshold is adaptive: starts at 256 KB and re-paces each minor, capped at 8192 KB. |
| `CLX_GC_MAJOR_KB` | Old-generation budget in kilobytes that triggers a full collection (default 32768). |
| `CLX_IO_NOBUF` | Set to `1` to bypass the runtime's userspace io.write batching (escape hatch / A/B). |

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Usage or compilation error |
| 2 | No C++ compiler found |
| *other* | On compiler failure, clx returns the underlying C++ compiler's exit code verbatim |

## Platform notes

- **Compiler**: the C++ compiler is fixed when clx is built (CMake uses the same
  compiler that built clx to compile your Lua scripts). This keeps toolchains
  consistent. On Windows clx builds with MSVC or with LLVM's `clang-cl`
  (`build.bat clang-cl`); `CLX_CXX=cl` / `CLX_CXX=clang-cl` switches the
  compiler for a single run.
- **Windows** outputs `.exe` / `.obj` / `.lib`; **Linux/macOS** outputs files
  with no extension / `.o` / `.a`.

## Building clx from source

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
./clx --help
```

(`./build.sh` and `./build.bat` wrap this. Without `-DCMAKE_BUILD_TYPE=Release` a
single-config generator gets no optimization flags.)

## About the optimization flags

If you're curious about exactly which compiler flags clx sets for each mode,
that detail is described in the [internals documentation](./internals/index.md).