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
| 3ddist.lua | 0.334s (1.00x) | 0.046s (7.26x) | **0.020s (16.70x)** |
| ackermann.lua | 0.174s (1.00x) | 0.026s (6.69x) | **0.017s (10.24x)** |
| arraysum.lua | 0.279s (1.00x) | **0.088s (3.17x)** | 0.164s (1.70x) |
| binarytrees.lua | 0.376s (1.00x) | 0.157s (2.39x) | **0.128s (2.94x)** |
| bubble.lua | 0.258s (1.00x) | **0.011s (23.45x)** | 0.027s (9.56x) |
| canada.lua | 0.269s (1.00x) | **0.106s (2.54x)** | 0.226s (1.19x) |
| coro.lua | 0.456s (1.00x) | **0.172s (2.65x)** | 0.492s (0.93x) |
| fannkuchredux.lua | 2.077s (1.00x) | **0.383s (5.42x)** | 1.306s (1.59x) |
| fasta.lua | 0.207s (1.00x) | **0.076s (2.72x)** | 0.134s (1.54x) |
| fib.lua | 0.295s (1.00x) | 0.048s (6.15x) | **0.016s (18.44x)** |
| hashtable.lua | 0.881s (1.00x) | **0.329s (2.68x)** | 0.423s (2.08x) |
| json.lua | 0.331s (1.00x) | 1.828s (0.18x) | **0.023s (14.39x)** |
| knucleotide.lua | 0.114s (1.00x) | **0.074s (1.54x)** | 0.100s (1.14x) |
| life.lua | 0.302s (1.00x) | 0.063s (4.79x) | **0.033s (9.15x)** |
| mandelbrot.lua | 0.195s (1.00x) | 0.027s (7.22x) | **0.025s (7.80x)** |
| nbody.lua | 0.179s (1.00x) | **0.018s (9.94x)** | 0.071s (2.52x) |
| pi.lua | 0.304s (1.00x) | 0.145s (2.10x) | **0.086s (3.53x)** |
| ray.lua | 0.350s (1.00x) | **0.021s (16.67x)** | 0.209s (1.67x) |
| realworld.lua | 0.689s (1.00x) | **0.318s (2.17x)** | 1.324s (0.52x) |
| regexdna.lua | 0.058s (1.00x) | 0.051s (1.14x) | **0.043s (1.35x)** |
| sieve.lua | 0.363s (1.00x) | **0.177s (2.05x)** | 0.291s (1.25x) |
| skynet.lua | 0.376s (1.00x) | **0.153s (2.46x)** | 0.161s (2.34x) |
| spectralnorm.lua | 0.310s (1.00x) | **0.016s (19.38x)** | 0.019s (16.32x) |
| warmup.lua | 0.004s (1.00x) | 0.003s (1.33x) | **0.002s (2.00x)** |

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Linux · GCC 15.2.0 · Average of 10 runs · clx `--fast` built with AVX2 (`CLX_ARCH=avx2`)

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
| 3ddist.lua | 0.342s (1.00x) | 0.083s (4.14x) | **0.066s (5.19x)** |
| ackermann.lua | 0.206s (1.00x) | **0.066s (3.11x)** | 0.109s (1.88x) |
| arraysum.lua | 0.270s (1.00x) | **0.127s (2.12x)** | 0.145s (1.86x) |
| binarytrees.lua | 0.465s (1.00x) | **0.181s (2.57x)** | 0.239s (1.95x) |
| bubble.lua | 0.284s (1.00x) | **0.050s (5.72x)** | 0.063s (4.52x) |
| canada.lua | 0.283s (1.00x) | **0.130s (2.17x)** | 0.253s (1.12x) |
| coro.lua | 4.654s (1.00x) | **0.219s (21.26x)** | 0.809s (5.75x) |
| fannkuchredux.lua | 1.884s (1.00x) | **0.424s (4.44x)** | 2.161s (0.87x) |
| fasta.lua | 1.607s (1.00x) | 1.352s (1.19x) | **0.216s (7.43x)** |
| fib.lua | 0.317s (1.00x) | 0.088s (3.58x) | **0.068s (4.66x)** |
| hashtable.lua | 0.920s (1.00x) | **0.322s (2.86x)** | 0.408s (2.26x) |
| json.lua | 0.648s (1.00x) | 2.085s (0.31x) | **0.068s (9.57x)** |
| knucleotide.lua | 0.145s (1.00x) | **0.105s (1.38x)** | 0.127s (1.14x) |
| life.lua | 0.315s (1.00x) | **0.104s (3.02x)** | 0.106s (2.97x) |
| mandelbrot.lua | 0.249s (1.00x) | 0.098s (2.53x) | **0.062s (4.04x)** |
| nbody.lua | 0.195s (1.00x) | **0.056s (3.49x)** | 0.122s (1.59x) |
| pi.lua | 0.323s (1.00x) | 0.198s (1.63x) | **0.123s (2.63x)** |
| ray.lua | 0.436s (1.00x) | **0.108s (4.03x)** | 0.422s (1.03x) |
| realworld.lua | 0.761s (1.00x) | **0.350s (2.18x)** | 1.514s (0.50x) |
| regexdna.lua | 0.090s (1.00x) | 0.086s (1.04x) | **0.085s (1.06x)** |
| sieve.lua | 0.374s (1.00x) | **0.226s (1.65x)** | 0.280s (1.34x) |
| skynet.lua | 1.475s (1.00x) | **0.220s (6.72x)** | 4.785s (0.31x) |
| spectralnorm.lua | 0.373s (1.00x) | **0.061s (6.09x)** | 0.078s (4.80x) |
| warmup.lua | **0.045s (1.00x)** | 0.048s (0.95x) | 0.048s (0.96x) |

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Windows 11 · MSVC 19.50.35724 · Average of 10 runs · clx `--fast` built with AVX2 (`CLX_ARCH=avx2`)

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
