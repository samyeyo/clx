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
| 3ddist.lua | 0.306s (1.00x) | 0.045s (6.80x) | **0.019s (16.11x)** |
| ackermann.lua | 0.160s (1.00x) | 0.026s (6.15x) | **0.016s (10.00x)** |
| arraysum.lua | 0.271s (1.00x) | **0.088s (3.08x)** | 0.102s (2.66x) |
| binarytrees.lua | 0.295s (1.00x) | **0.155s (1.90x)** | 0.199s (1.48x) |
| bubble.lua | 0.253s (1.00x) | **0.011s (23.00x)** | 0.078s (3.24x) |
| canada.lua | 0.248s (1.00x) | **0.102s (2.43x)** | 0.213s (1.16x) |
| coro.lua | 0.460s (1.00x) | **0.172s (2.67x)** | 0.327s (1.41x) |
| fannkuchredux.lua | 1.946s (1.00x) | **0.386s (5.04x)** | 1.105s (1.76x) |
| fasta.lua | 0.213s (1.00x) | **0.070s (3.04x)** | 0.099s (2.15x) |
| fib.lua | 0.286s (1.00x) | 0.051s (5.61x) | **0.006s (47.67x)** |
| hashtable.lua | 0.839s (1.00x) | **0.304s (2.76x)** | 0.438s (1.92x) |
| json.lua | 0.321s (1.00x) | 1.852s (0.17x) | **0.025s (12.84x)** |
| knucleotide.lua | 0.101s (1.00x) | **0.067s (1.51x)** | 0.102s (0.99x) |
| life.lua | 0.284s (1.00x) | **0.064s (4.44x)** | 0.067s (4.24x) |
| mandelbrot.lua | 0.190s (1.00x) | 0.025s (7.60x) | **0.022s (8.64x)** |
| nbody.lua | 0.168s (1.00x) | **0.016s (10.50x)** | 0.069s (2.43x) |
| pi.lua | 0.282s (1.00x) | 0.135s (2.09x) | **0.063s (4.48x)** |
| regexdna.lua | 0.054s (1.00x) | 0.046s (1.17x) | **0.041s (1.32x)** |
| sieve.lua | 0.329s (1.00x) | **0.162s (2.03x)** | 0.217s (1.52x) |
| skynet.lua | 0.371s (1.00x) | **0.156s (2.38x)** | 0.172s (2.16x) |
| spectralnorm.lua | 0.316s (1.00x) | **0.017s (18.59x)** | 0.029s (10.90x) |
| warmup.lua | 0.004s (1.00x) | 0.004s (1.00x) | **0.002s (2.00x)** |

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