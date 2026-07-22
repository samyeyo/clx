# clx Command-Line Interface

## Usage

```bash
clx [options] <file.lua> [<compiler-options>]
```

Options starting with `-` that are not recognized by clx are automatically passed through to the C++ compiler.

## Options

### Build Mode

| Option | Description |
|--------|-------------|
| `--executable` | Compile to executable (default) |
| `--object` | Compile to object file (.o/.obj) |
| `--static` | Compile to static library (.a/.lib) |

### Output Options

| Option | Description |
|--------|-------------|
| `--output <name>` | Specify output file name |

### Compilation Options

| Option | Description |
|--------|-------------|
| `--debug` | Enable debug symbols, disable optimizations; `#line` directives map debugger views to Lua source lines and filenames |
| `--size` | Optimize for size (default: `-Os` or `/O1` with MSVC) |
| `--fast` | Optimize for speed (`-O3` or `/O2 /Ot` with MSVC) |
| `--cpp` | Generate C++ source files, don't compile |
| `--minimal` | Exclude non-essential modules (string, table, io, os, math, utf8, coroutine); keeps base + package |
| `--modules <list>` | Precompiled modules to link (comma-separated list) |

### Other

| Option | Description |
|--------|-------------|
| `--help` | Display help message |
| `--version` | Print version and copyright |

### Pass-Through Options

Any option starting with `-` that is not recognized by clx is passed directly to the underlying C++ compiler. This allows you to:

```bash
# Optimize for speed (disables default flags)
clx file.lua -O2

# Target specific CPU (disables default flags)
clx file.lua -march=native

# Combine multiple options (disables default flags)
clx file.lua -O2 -march=native
```

## Examples

### Basic Compilation

```bash
# Compile to executable named "myapp"
clx script.lua

# Run the executable
./myapp
```

### Specify Output Name

```bash
clx script.lua --output myprogram
./myprogram
```

### Debug Build

```bash
clx script.lua --debug
```

Produces a debuggable executable. The generated C++ contains `#line` directives mapping each statement back to the original Lua file and line number, so GDB, LLDB, or MSVC debugger can step through the `.lua` source and show Lua stack traces.

### Keep Generated C++

```bash
clx script.lua --cpp
# Generates script.cpp without compiling
```

### Object File

```bash
clx script.lua --object
# Produces script.o (or script.obj on Windows)
```

### Static Library

```bash
clx script.lua --static
# Produces script.a (or script.lib on Windows)
```

## Optimization Flags

Default optimization flags are only applied when no compiler options are provided by the user.

### Size Mode (`--size`, default)
Optimizes for binary size:
- **gcc/clang**: `-Os -flto=auto -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -Wl,--gc-sections -s`
- **MSVC**: `/O1 /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /link /OPT:REF /OPT:ICF`

`--size` links against `libclx_size.a` (runtime compiled with `-Os`) and reduces binary size by **36–40%** across typical scripts compared to `--fast`.

**Performance tradeoff vs `--fast`:**
- Compute-bound code (fib, ackermann, bubble, pi): 80–320% slower
- Table/memory-bound code (nbody, hashtable, arraysum): negligible difference

Use `--fast` for compute-heavy Lua code where throughput matters more than binary size.

### Fast Mode (`--fast`)
Optimizes for execution speed:
- **gcc/clang**: `-O3 -flto=auto -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -Wl,--gc-sections -s`
- **MSVC**: `/O2 /Ot /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /link /OPT:REF /OPT:ICF`

### Debug Mode (`--debug`)
- **gcc/clang**: `-O0 -g`
- **MSVC**: `/Od /Zi /MDd /EHsc`
- Emitted `#line` directives preserve Lua source mapping in debug symbols — debuggers show `.lua` filenames and line numbers

### User-Provided Flags
If you provide any compiler options (e.g., `-O2`, `/O2`, `-march=native`), no default optimization flags are added. This gives you full control over the compilation.

## Environment Variables

clx respects these environment variables:

| Variable | Description |
|----------|-------------|
| `CXX` | (Not read — compiler fixed at build time) |

## Exit Codes

| Code | Description |
|------|-------------|
| 0 | Success |
| 1 | Usage or compilation error |

## Platform-Specific

### Linux/macOS

- Compiler is fixed at build time via CMake (the same compiler that built `clx` is used for Lua script compilation).
- Both GCC and Clang support tail-call optimization (TCO) via `CLX_MUSTTAIL`. On Clang this uses `[[clang::musttail]]`, on GCC it uses `[[gnu::musttail]]`.

### Windows

- Compiler is fixed at build time via CMake (the same compiler that built `clx` is used for Lua script compilation).
- Output executable: `<name>.exe`
- Object file: `<name>.obj`
- Static library: `<name>.lib`

## Build with CMake

If building from source:

```bash
mkdir build
cd build
cmake ..
make
./clx --help
```