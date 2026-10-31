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
| 3ddist.lua | 0.346s (1.00x) | 0.050s (6.92x) | **0.023s (15.04x)** |
| ackermann.lua | 0.183s (1.00x) | 0.032s (5.72x) | **0.018s (10.17x)** |
| arraysum.lua | 0.300s (1.00x) | 0.095s (3.16x) | **0.093s (3.23x)** |
| binarytrees.lua | 0.339s (1.00x) | **0.177s (1.92x)** | 0.190s (1.78x) |
| bubble.lua | 0.289s (1.00x) | **0.015s (19.27x)** | 0.075s (3.85x) |
| canada.lua | 0.398s (1.00x) | **0.132s (3.02x)** | 0.219s (1.82x) |
| coro.lua | 0.516s (1.00x) | **0.199s (2.59x)** | 0.454s (1.14x) |
| fannkuchredux.lua | 2.355s (1.00x) | **0.431s (5.46x)** | 1.271s (1.85x) |
| fasta.lua | 0.236s (1.00x) | **0.077s (3.06x)** | 0.112s (2.11x) |
| fib.lua | 0.319s (1.00x) | 0.051s (6.25x) | **0.007s (45.57x)** |
| hashtable.lua | 0.997s (1.00x) | **0.385s (2.59x)** | 0.441s (2.26x) |
| json.lua | 0.371s (1.00x) | 2.089s (0.18x) | **0.026s (14.27x)** |
| knucleotide.lua | 0.137s (1.00x) | **0.093s (1.47x)** | 0.115s (1.19x) |
| life.lua | 0.332s (1.00x) | **0.061s (5.44x)** | 0.062s (5.35x) |
| mandelbrot.lua | 0.228s (1.00x) | 0.031s (7.35x) | **0.026s (8.77x)** |
| nbody.lua | 0.198s (1.00x) | **0.021s (9.43x)** | 0.101s (1.96x) |
| pi.lua | 0.333s (1.00x) | 0.162s (2.06x) | **0.074s (4.50x)** |
| regexdna.lua | 0.080s (1.00x) | 0.062s (1.29x) | **0.051s (1.57x)** |
| sieve.lua | 0.389s (1.00x) | **0.185s (2.10x)** | 0.220s (1.77x) |
| skynet.lua | 0.407s (1.00x) | **0.182s (2.24x)** | 0.285s (1.43x) |
| spectralnorm.lua | 0.339s (1.00x) | **0.020s (16.95x)** | 0.021s (16.14x) |
| warmup.lua | 0.006s (1.00x) | 0.005s (1.20x) | **0.002s (3.00x)** |

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Linux · GCC 15.2.0 · Average of 10 runs · clx `--fast` built with AVX2 (`CLX_ARCH=avx2` — the default x86 build is SSE2)

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
| 3ddist.lua | 0.343s (1.00x) | 0.088s (3.92x) | **0.066s (5.19x)** |
| ackermann.lua | 0.206s (1.00x) | **0.065s (3.18x)** | 0.109s (1.88x) |
| arraysum.lua | 0.294s (1.00x) | 0.149s (1.97x) | **0.116s (2.53x)** |
| binarytrees.lua | 0.524s (1.00x) | 0.210s (2.50x) | **0.205s (2.55x)** |
| bubble.lua | 0.293s (1.00x) | **0.048s (6.05x)** | 0.058s (5.04x) |
| canada.lua | 0.317s (1.00x) | **0.136s (2.33x)** | 0.255s (1.25x) |
| coro.lua | 4.599s (1.00x) | **0.212s (21.72x)** | 0.621s (7.40x) |
| fannkuchredux.lua | 1.888s (1.00x) | **0.436s (4.33x)** | 2.054s (0.92x) |
| fasta.lua | 1.619s (1.00x) | 1.378s (1.17x) | **0.215s (7.53x)** |
| fib.lua | 0.331s (1.00x) | 0.089s (3.72x) | **0.067s (4.96x)** |
| hashtable.lua | 0.943s (1.00x) | **0.336s (2.80x)** | 0.421s (2.24x) |
| json.lua | 0.718s (1.00x) | 2.161s (0.33x) | **0.065s (10.99x)** |
| knucleotide.lua | 0.148s (1.00x) | **0.098s (1.51x)** | 0.130s (1.14x) |
| life.lua | 0.315s (1.00x) | **0.102s (3.11x)** | 0.246s (1.28x) |
| mandelbrot.lua | 0.262s (1.00x) | 0.103s (2.53x) | **0.065s (4.03x)** |
| nbody.lua | 0.191s (1.00x) | **0.062s (3.11x)** | 0.123s (1.56x) |
| pi.lua | 0.341s (1.00x) | 0.205s (1.66x) | **0.120s (2.83x)** |
| ray.lua | 0.436s (1.00x) | **0.112s (3.88x)** | 0.414s (1.05x) |
| realworld.lua | 0.814s (1.00x) | **0.357s (2.28x)** | 1.361s (0.60x) |
| regexdna.lua | 0.091s (1.00x) | 0.091s (1.00x) | **0.091s (1.00x)** |
| sieve.lua | 0.357s (1.00x) | **0.200s (1.79x)** | 0.224s (1.60x) |
| skynet.lua | 1.407s (1.00x) | **0.211s (6.68x)** | 0.321s (4.39x) |
| spectralnorm.lua | 0.351s (1.00x) | **0.061s (5.72x)** | 0.066s (5.34x) |
| warmup.lua | 0.048s (1.00x) | 0.049s (0.97x) | **0.043s (1.13x)** |

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
