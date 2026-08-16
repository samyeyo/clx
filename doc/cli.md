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
| `--output <name>` | Set the output file's name |

### Control the build

| Option | What it does |
|--------|--------------|
| `--debug` | Keep debugging information so you can set breakpoints in the Lua source |
| `--size` | Optimize for a small binary (default) |
| `--fast` | Optimize for speed instead of size |
| `--cpp` | Write out the generated C++ files without compiling (for debugging) |
| `--minimal` | Leave out the non-essential libraries (string, table, io, os, math, utf8, coroutine) |
| `--dynamic` | Enable `load`, `loadfile`, and `dofile` so code can run at runtime |
| `--modules <list>` | Link prebuilt native modules (comma-separated) |

### Help

| Option | What it does |
|--------|--------------|
| `--help` | Show the help message |
| `--version` | Print the version and copyright |

## Pass-through options

Anything starting with `-` that clx doesn't recognize is handed to the C++
compiler. This is useful for targeting a specific CPU or tuning things yourself.
Note that using any of these turns off clx's default optimization flags.

```bash
# Optimize for speed (replaces the default flags)
clx file.lua -O2

# Target a specific CPU (replaces the default flags)
clx file.lua -march=native

# Combine several options (replaces the default flags)
clx file.lua -O2 -march=native
```

## Common examples

### Compile and run

```bash
clx script.lua
./myapp            # the output is named after script.lua by default
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
| `CXX` | Not used — the C++ compiler is chosen when clx is built, not at runtime. |

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Usage or compilation error |

## Platform notes

- **Compiler**: the C++ compiler is fixed when clx is built (CMake uses the same
  compiler that built clx to compile your Lua scripts). This keeps toolchains
  consistent.
- **Windows** outputs `.exe` / `.obj` / `.lib`; **Linux/macOS** outputs files
  with no extension / `.o` / `.a`.

## Building clx from source

```bash
mkdir build
cd build
cmake ..
make
./clx --help
```

## About the optimization flags

If you're curious about exactly which compiler flags clx sets for each mode,
that detail is described in the [internals documentation](./internals/index.md).