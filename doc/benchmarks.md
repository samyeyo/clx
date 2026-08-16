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
| 3ddist.lua | 0.506s (1.00x) | 0.078s (6.49x) | **0.048s (10.54x)** |
| ackermann.lua | 0.175s (1.00x) | 0.027s (6.48x) | **0.019s (9.21x)** |
| arraysum.lua | 0.317s (1.00x) | 0.098s (3.23x) | **0.083s (3.82x)** |
| binarytrees.lua | 0.344s (1.00x) | **0.171s (2.01x)** | 0.276s (1.25x) |
| bubble.lua | 0.269s (1.00x) | **0.014s (19.21x)** | 0.057s (4.72x) |
| canada.lua | 0.347s (1.00x) | **0.134s (2.59x)** | 0.231s (1.50x) |
| coro.lua | 0.471s (1.00x) | **0.184s (2.56x)** | 0.354s (1.33x) |
| fannkuchredux.lua | 2.472s (1.00x) | **0.458s (5.40x)** | 1.090s (2.27x) |
| fasta.lua | 0.295s (1.00x) | **0.132s (2.23x)** | 0.230s (1.28x) |
| fib.lua | 0.299s (1.00x) | 0.044s (6.80x) | **0.007s (42.71x)** |
| hashtable.lua | 0.991s (1.00x) | **0.386s (2.57x)** | 0.617s (1.61x) |
| json.lua | 0.358s (1.00x) | 1.864s (0.19x) | **0.033s (10.85x)** |
| knucleotide.lua | 0.114s (1.00x) | **0.073s (1.56x)** | 0.094s (1.21x) |
| life.lua | 0.299s (1.00x) | 0.069s (4.33x) | **0.058s (5.16x)** |
| mandelbrot.lua | 0.201s (1.00x) | 0.027s (7.44x) | **0.005s (40.20x)** |
| nbody.lua | 0.179s (1.00x) | **0.017s (10.53x)** | 0.086s (2.08x) |
| pi.lua | 0.302s (1.00x) | 0.145s (2.08x) | **0.069s (4.38x)** |
| sieve.lua | 0.364s (1.00x) | 0.171s (2.13x) | **0.166s (2.19x)** |
| spectralnorm.lua | 0.382s (1.00x) | **0.020s (19.10x)** | 0.036s (10.61x) |
| warmup.lua | **0.003s (1.00x)** | 0.005s (0.60x) | 0.006s (0.50x) |

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