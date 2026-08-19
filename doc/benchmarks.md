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

> Windows users: create a `lua\` folder in the clx root containing `lua55.exe` /
> `luajit.exe` plus their `lua55.dll` / `lua51.dll` libraries.

## Running the suite

### Linux / macOS

From the project root:

```sh
./benchmarks/run.sh
```

The script compiles each benchmark with clx, times all three engines over 10
runs (plus 3 warmup), pins to a single CPU core when possible, and prints a
formatted speedup table.

To tweak the C++ compiler flags (for example, a different optimization level):

```sh
CPPFLAGS="-O2" ./benchmarks/run.sh
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

Speedups vs. Lua 5.5 (10-run average, single CPU, `hyperfine`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.378s (1.00x) | 0.052s (7.27x) | **0.021s (18.00x)** |
| ackermann.lua | 0.228s (1.00x) | 0.027s (8.44x) | **0.017s (13.41x)** |
| arraysum.lua | 0.284s (1.00x) | 0.088s (3.23x) | **0.070s (4.06x)** |
| binarytrees.lua | 0.329s (1.00x) | **0.175s (1.88x)** | 0.262s (1.26x) |
| bubble.lua | 0.344s (1.00x) | **0.012s (28.67x)** | 0.052s (6.62x) |
| canada.lua | 0.300s (1.00x) | **0.120s (2.50x)** | 0.233s (1.29x) |
| coro.lua | 0.490s (1.00x) | **0.182s (2.69x)** | 0.351s (1.40x) |
| fannkuchredux.lua | 2.130s (1.00x) | **0.411s (5.18x)** | 0.942s (2.26x) |
| fasta.lua | 0.244s (1.00x) | **0.071s (3.44x)** | 0.113s (2.16x) |
| fib.lua | 0.311s (1.00x) | 0.042s (7.40x) | **0.008s (38.88x)** |
| hashtable.lua | 0.927s (1.00x) | **0.352s (2.63x)** | 0.440s (2.11x) |
| json.lua | 0.352s (1.00x) | 1.948s (0.18x) | **0.026s (13.54x)** |
| knucleotide.lua | 0.111s (1.00x) | **0.074s (1.50x)** | 0.103s (1.08x) |
| life.lua | 0.302s (1.00x) | **0.054s (5.59x)** | 0.070s (4.31x) |
| mandelbrot.lua | 0.202s (1.00x) | **0.026s (7.77x)** | 0.023s (8.78x) |
| nbody.lua | 0.178s (1.00x) | **0.019s (9.37x)** | 0.069s (2.58x) |
| pi.lua | 0.308s (1.00x) | 0.145s (2.12x) | **0.067s (4.60x)** |
| sieve.lua | 0.364s (1.00x) | **0.172s (2.12x)** | 0.174s (2.09x) |
| skynet.lua | 0.391s (1.00x) | 0.164s (2.38x) | **0.160s (2.44x)** |
| spectralnorm.lua | 0.343s (1.00x) | **0.018s (19.06x)** | 0.032s (10.72x) |
| warmup.lua | 0.003s (1.00x) | 0.004s (0.75x) | **0.002s (1.50x)** |

> Measured on an Intel® Core™ i5 Ultra 125U CPU @ 4.30GHz · Linux · GCC 13.3.0

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
| **json** | JSON encoder (dkjson) — strings, recursion, mixed data |
| **coro** | Coroutine yield cycles — coroutine overhead |
| **canada** | Parsing a real 2.2 MB GeoJSON file — the most realistic workload |
| **skynet** | Actor-model coroutine stress test — millions of coroutine create/resume/yield cycles |
| **warmup** | A trivial script — startup latency only |

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

A minimal script that just allocates a tiny bit and exits. It measures how long
the runtime takes to start up and reach the first statement, rather than
execution speed.