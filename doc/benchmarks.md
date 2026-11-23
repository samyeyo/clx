# Benchmarks

clx ships a suite of benchmarks under the `benchmarks/` directory. Each is a
self-contained Lua script you can run with plain Lua, LuaJIT, or as a
clx-compiled binary, so the three can be compared directly.

## What you need

| Tool | Purpose |
|---|---|
| `lua` (5.5) | Reference interpreter |
| `luajit` (2.1) | JIT comparison |
| `clx` | Built from source — see [Getting Started](getting-started.md) |
| `hyperfine` (POSIX only) | Optional, for the manual timing workflow |

> Windows users: create a `lua\` folder in the clx root containing `lua.exe` /
> `luajit.exe` plus their DLLs (`lua55.dll` / `lua51.dll`). The runner invokes
> the commands `lua` and `luajit`, so the binaries must be named accordingly.

## Running the suite

### Linux / macOS

From the project root:

```sh
./benchmarks/run.sh
```

The script compiles each benchmark with clx, times all three engines over 10
runs (plus 3 warmup), pins to a single CPU core when possible, and prints a
formatted speedup table.

To tweak the C++ compiler flags (for example, a different optimization level),
set `CPPFLAGS` — note it **replaces** the default `--fast`, so keep that in the
value if you still want the speed-linked runtime:

```sh
CPPFLAGS="--fast -O2" ./benchmarks/run.sh
```

A `hyperfine`-based variant is also provided:

```sh
./benchmarks/run-hyperfine.sh
```

### Windows

Run from a Developer Command Prompt with MSVC. A `lua\` folder with the
interpreters must exist as described above.

```bat
cd \path\to\clx
benchmarks\run.bat
```

## Current results

### Linux x86-64

Speedups vs. Lua 5.5 (10-run average, single CPU, `hyperfine`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.352s (1.00x) | 0.057s (6.19x) | **0.024s (14.43x)** |
| ackermann.lua | 0.190s (1.00x) | 0.030s (6.42x) | **0.018s (10.50x)** |
| arraysum.lua | 0.333s (1.00x) | **0.097s (3.43x)** | 0.126s (2.64x) |
| binarytrees.lua | 0.331s (1.00x) | 0.190s (1.74x) | **0.120s (2.75x)** |
| bubble.lua | 0.289s (1.00x) | **0.014s (20.50x)** | 0.029s (9.90x) |
| canada.lua | 0.337s (1.00x) | **0.128s (2.63x)** | 0.264s (1.27x) |
| coro.lua | 0.534s (1.00x) | **0.201s (2.65x)** | 0.443s (1.21x) |
| fannkuchredux.lua | 2.298s (1.00x) | **0.440s (5.22x)** | 1.477s (1.56x) |
| fasta.lua | 0.259s (1.00x) | **0.080s (3.23x)** | 0.153s (1.69x) |
| fib.lua | 0.330s (1.00x) | 0.061s (5.40x) | **0.020s (16.48x)** |
| hashtable.lua | 0.934s (1.00x) | **0.373s (2.50x)** | 0.519s (1.80x) |
| json.lua | 0.351s (1.00x) | 2.012s (0.17x) | **0.028s (12.69x)** |
| knucleotide.lua | 0.136s (1.00x) | **0.093s (1.46x)** | 0.116s (1.17x) |
| life.lua | 0.321s (1.00x) | 0.056s (5.77x) | **0.043s (7.50x)** |
| mandelbrot.lua | 0.220s (1.00x) | **0.029s (7.60x)** | 0.030s (7.22x) |
| nbody.lua | 0.184s (1.00x) | **0.023s (7.97x)** | 0.086s (2.13x) |
| pi.lua | 0.341s (1.00x) | 0.164s (2.08x) | **0.097s (3.53x)** |
| ray.lua | 0.389s (1.00x) | **0.027s (14.64x)** | 0.226s (1.72x) |
| realworld.lua | 0.727s (1.00x) | **0.337s (2.15x)** | 1.391s (0.52x) |
| regexdna.lua | 0.066s (1.00x) | 0.055s (1.21x) | **0.048s (1.38x)** |
| sieve.lua | 0.386s (1.00x) | **0.171s (2.26x)** | 0.238s (1.63x) |
| skynet.lua | 0.432s (1.00x) | **0.182s (2.38x)** | 0.188s (2.30x) |
| spectralnorm.lua | 0.339s (1.00x) | **0.021s (16.27x)** | 0.022s (15.32x) |
| warmup.lua | **0.004s (1.00x)** | 0.008s (0.43x) | 0.005s (0.71x) |

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Windows 11 + WSL2 (Ubuntu 24.04) · GCC 15.2.0 · Average of 10 runs · clx `--fast` built with the portable baseline (`CLX_ARCH` unset)

### macOS ARM64

Speedups vs. Lua 5.5 (10-run average, `hyperfine`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.384s (1.00x) | 0.041s (9.37x) | **0.013s (29.54x)** |
| ackermann.lua | 0.159s (1.00x) | **0.022s (7.23x)** | 0.049s (3.24x) |
| arraysum.lua | 0.205s (1.00x) | 0.066s (3.11x) | **0.053s (3.87x)** |
| binarytrees.lua | 0.391s (1.00x) | 0.111s (3.52x) | **0.097s (4.03x)** |
| bubble.lua | 0.278s (1.00x) | **0.013s (21.38x)** | 0.025s (11.12x) |
| canada.lua | 0.169s (1.00x) | **0.082s (2.06x)** | 0.124s (1.36x) |
| coro.lua | 0.525s (1.00x) | **0.097s (5.41x)** | 0.247s (2.13x) |
| fannkuchredux.lua | 1.954s (1.00x) | **0.277s (7.05x)** | 0.682s (2.87x) |
| fasta.lua | 0.237s (1.00x) | **0.082s (2.89x)** | 0.092s (2.58x) |
| fib.lua | 0.253s (1.00x) | 0.032s (7.91x) | **0.025s (10.12x)** |
| hashtable.lua | 0.330s (1.00x) | **0.096s (3.44x)** | 0.166s (1.99x) |
| json.lua | 0.133s (1.00x) | 0.540s (0.25x) | **0.016s (8.31x)** |
| knucleotide.lua | 0.073s (1.00x) | **0.037s (1.97x)** | 0.041s (1.78x) |
| life.lua | 0.320s (1.00x) | **0.032s (10.00x)** | 0.053s (6.04x) |
| mandelbrot.lua | 0.279s (1.00x) | 0.025s (11.16x) | **0.021s (13.29x)** |
| nbody.lua | 0.215s (1.00x) | **0.012s (17.92x)** | 0.058s (3.71x) |
| pi.lua | 0.384s (1.00x) | 0.543s (0.71x) | **0.050s (7.68x)** |
| ray.lua | 0.405s (1.00x) | **0.019s (21.32x)** | 0.234s (1.73x) |
| realworld.lua | 0.608s (1.00x) | **0.260s (2.34x)** | 0.483s (1.26x) |
| regexdna.lua | 0.047s (1.00x) | 0.045s (1.04x) | **0.034s (1.38x)** |
| sieve.lua | 0.208s (1.00x) | 0.222s (0.94x) | **0.108s (1.93x)** |
| skynet.lua | 0.454s (1.00x) | **0.122s (3.72x)** | 0.094s (4.83x) |
| spectralnorm.lua | 0.363s (1.00x) | **0.015s (24.20x)** | 0.019s (19.11x) |
| warmup.lua | 0.005s (1.00x) | 0.004s (1.25x) | **0.004s (1.25x)** |

> Measured on an Apple A18 Pro · macOS 26.6.2 (ARM64) · Apple clang 21.0.0 · Average of 10 runs · clx `--fast`

### Windows x86-64 (MSVC cl.exe)

Speedups vs. Lua 5.5 (10-run average, `benchmarks\run.bat`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.351s (1.00x) | 0.067s (5.23x) | **0.052s (6.72x)** |
| ackermann.lua | 0.200s (1.00x) | **0.058s (3.46x)** | 0.102s (1.96x) |
| arraysum.lua | 0.288s (1.00x) | 0.127s (2.27x) | **0.114s (2.53x)** |
| binarytrees.lua | 0.505s (1.00x) | **0.188s (2.69x)** | 0.252s (2.00x) |
| bubble.lua | 0.298s (1.00x) | **0.036s (8.34x)** | 0.054s (5.56x) |
| canada.lua | 0.305s (1.00x) | **0.130s (2.34x)** | 0.276s (1.11x) |
| coro.lua | 5.366s (1.00x) | **0.224s (23.96x)** | 0.748s (7.17x) |
| fannkuchredux.lua | 2.031s (1.00x) | **0.457s (4.44x)** | 2.412s (0.84x) |
| fasta.lua | 1.771s (1.00x) | 1.515s (1.17x) | **0.218s (8.11x)** |
| fib.lua | 0.339s (1.00x) | 0.075s (4.51x) | **0.058s (5.82x)** |
| hashtable.lua | 1.135s (1.00x) | **0.369s (3.07x)** | 0.513s (2.21x) |
| json.lua | 0.740s (1.00x) | 2.243s (0.33x) | **0.051s (14.44x)** |
| knucleotide.lua | 0.138s (1.00x) | **0.083s (1.65x)** | 0.130s (1.06x) |
| life.lua | 0.329s (1.00x) | **0.090s (3.67x)** | 0.103s (3.20x) |
| mandelbrot.lua | 0.266s (1.00x) | 0.098s (2.73x) | **0.065s (4.08x)** |
| nbody.lua | 0.207s (1.00x) | **0.044s (4.67x)** | 0.127s (1.63x) |
| pi.lua | 0.346s (1.00x) | 0.221s (1.57x) | **0.167s (2.07x)** |
| ray.lua | 0.464s (1.00x) | **0.115s (4.03x)** | 0.458s (1.01x) |
| realworld.lua | 0.856s (1.00x) | **0.385s (2.22x)** | 1.718s (0.50x) |
| regexdna.lua | 0.081s (1.00x) | 0.080s (1.00x) | **0.073s (1.10x)** |
| sieve.lua | 0.364s (1.00x) | **0.190s (1.91x)** | 0.228s (1.60x) |
| skynet.lua | 1.524s (1.00x) | **0.218s (6.99x)** | 0.335s (4.55x) |
| spectralnorm.lua | 0.358s (1.00x) | **0.041s (8.81x)** | 0.057s (6.23x) |
| warmup.lua | **0.029s (1.00x)** | 0.031s (0.95x) | 0.030s (0.98x) |

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Windows 11 · MSVC 19.50.35717 · Average of 10 runs · clx `--fast` built with AVX2 (`CLX_ARCH=avx2`)

## What each benchmark tests

| Script | What it stresses |
|--------|------------------|
| **fib** | Recursive Fibonacci — function-call overhead and simple arithmetic |
| **ackermann** | Deep recursion — call overhead and stack handling |
| **spectralnorm** | Dense math with arrays — numeric type inference |
| **nbody** | Planetary gravity simulation — float math and struct-like tables |
| **mandelbrot** | Mandelbrot set — tight math loop with early exits |
| **fannkuchredux** | Permutation counting — heavy table access and integer math |
| **binarytrees** | Millions of tree nodes — garbage-collection speed |
| **knucleotide** | DNA sequence counting — string hashing and table inserts |
| **fasta** | DNA sequence generation — math, string building, and I/O |
| **bubble** | Bubble sort — array read/write throughput |
| **arraysum** | Summing a large numeric array — loop and array-read speed |
| **hashtable** | String-keyed insert/read — string interning and hashing |
| **pi** | Monte Carlo π — tight integer/float arithmetic |
| **3ddist** | Distance math — `math` library dispatch and float math |
| **life** | Conway's Game of Life — 2D array traversal |
| **sieve** | Prime sieve — array writes and branching |
| **json** | Hand-rolled JSON encoder — strings, recursion, mixed data (no third-party dependency) |
| **coro** | Coroutine yield cycles — coroutine overhead |
| **canada** | Parsing a real 2.2 MB GeoJSON file with dkjson — the most realistic workload |
| **skynet** | Actor-model coroutine stress test — millions of coroutine create/resume/yield cycles |
| **ray** | Ray tracer — float math, table-heavy scene traversal |
| **realworld** | Mixed real-world workload (parse + transform + aggregate) |
| **warmup** | A one-line script — startup latency only |

### About `canada.lua`

This parses the canonical `canada.json` GeoJSON dataset with
[dkjson](http://dkolf.de/src/dkjson-lua.fsl/home) and walks every coordinate to
find the bounding box. It's the most representative real-world test: real file
I/O, big strings, a third-party library, and deep table traversal.

**Required files** (both must be in the working directory):

| File | Source |
|---|---|
| `dkjson.lua` | <http://dkolf.de/src/dkjson-lua.fsl/home> |
| `canada.json` | <https://github.com/nicholasgasior/gsfmt/blob/master/testdata/canada.json> |

Expected output (all three engines must agree):

```
features:     1
total points: 55563
bbox x:       [-141.002991, -52.614449]
bbox y:       [41.675552, 83.113876]
```

### About `warmup.lua`

A one-line script (`local x = 5`). It measures how long the runtime takes to start
up and reach the first statement, rather than execution speed.
