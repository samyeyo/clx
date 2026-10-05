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

### Linux x86-64

Speedups vs. Lua 5.5 (10-run average, single CPU, `hyperfine`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.346s (1.00x) | 0.049s (7.06x) | **0.022s (15.73x)** |
| ackermann.lua | 0.182s (1.00x) | 0.029s (6.28x) | **0.019s (9.58x)** |
| arraysum.lua | 0.311s (1.00x) | **0.105s (2.96x)** | 0.117s (2.66x) |
| binarytrees.lua | 0.370s (1.00x) | **0.202s (1.83x)** | 0.216s (1.71x) |
| bubble.lua | 0.308s (1.00x) | **0.015s (20.53x)** | 0.083s (3.71x) |
| canada.lua | 0.392s (1.00x) | **0.150s (2.61x)** | 0.258s (1.52x) |
| coro.lua | 0.507s (1.00x) | **0.200s (2.53x)** | 0.436s (1.16x) |
| fannkuchredux.lua | 2.618s (1.00x) | **0.411s (6.37x)** | 1.359s (1.93x) |
| fasta.lua | 0.229s (1.00x) | **0.079s (2.90x)** | 0.119s (1.92x) |
| fib.lua | 0.327s (1.00x) | 0.055s (5.95x) | **0.007s (46.71x)** |
| hashtable.lua | 1.036s (1.00x) | **0.366s (2.83x)** | 0.445s (2.33x) |
| json.lua | 0.394s (1.00x) | 2.230s (0.18x) | **0.026s (15.15x)** |
| knucleotide.lua | 0.153s (1.00x) | **0.099s (1.55x)** | 0.108s (1.42x) |
| life.lua | 0.321s (1.00x) | **0.052s (6.17x)** | 0.088s (3.65x) |
| mandelbrot.lua | 0.216s (1.00x) | 0.029s (7.45x) | **0.026s (8.31x)** |
| nbody.lua | 0.200s (1.00x) | **0.020s (10.00x)** | 0.083s (2.41x) |
| pi.lua | 0.336s (1.00x) | 0.162s (2.07x) | **0.075s (4.48x)** |
| regexdna.lua | 0.075s (1.00x) | 0.057s (1.32x) | **0.053s (1.42x)** |
| sieve.lua | 0.376s (1.00x) | **0.184s (2.04x)** | 0.235s (1.60x) |
| skynet.lua | 0.417s (1.00x) | **0.175s (2.38x)** | 0.219s (1.90x) |
| spectralnorm.lua | 0.370s (1.00x) | **0.022s (16.82x)** | 0.034s (10.88x) |
| warmup.lua | 0.007s (1.00x) | 0.006s (1.17x) | **0.003s (2.33x)** |

> Measured on an Intel® Core™ i5 Ultra 125U CPU @ 4.30GHz · Linux · GCC 13.3.0 · Average of 10 runs · clx `--fast` built with AVX2

### macOS ARM64

Speedups vs. Lua 5.5 (10-run average, `hyperfine`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.406s (1.00x) | 0.043s (9.44x) | **0.015s (27.07x)** |
| ackermann.lua | 0.172s (1.00x) | **0.030s (5.73x)** | 0.050s (3.44x) |
| arraysum.lua | 0.216s (1.00x) | 0.056s (3.86x) | **0.053s (4.08x)** |
| binarytrees.lua | 0.395s (1.00x) | **0.111s (3.56x)** | 0.124s (3.19x) |
| bubble.lua | 0.286s (1.00x) | **0.013s (22.00x)** | 0.051s (5.61x) |
| canada.lua | 0.172s (1.00x) | **0.091s (1.89x)** | 0.144s (1.19x) |
| coro.lua | 0.527s (1.00x) | **0.101s (5.22x)** | 0.325s (1.62x) |
| fannkuchredux.lua | 1.973s (1.00x) | **0.302s (6.53x)** | 0.797s (2.48x) |
| fasta.lua | 0.251s (1.00x) | **0.086s (2.92x)** | 0.147s (1.71x) |
| fib.lua | 0.254s (1.00x) | 0.039s (6.51x) | **0.026s (9.77x)** |
| hashtable.lua | 0.453s (1.00x) | **0.147s (3.08x)** | 0.191s (2.37x) |
| json.lua | 0.141s (1.00x) | 0.600s (0.23x) | **0.019s (7.42x)** |
| knucleotide.lua | 0.077s (1.00x) | **0.038s (2.03x)** | 0.044s (1.75x) |
| life.lua | 0.329s (1.00x) | **0.030s (10.97x)** | 0.046s (7.15x) |
| mandelbrot.lua | 0.286s (1.00x) | 0.025s (11.44x) | **0.023s (12.43x)** |
| nbody.lua | 0.220s (1.00x) | **0.014s (15.71x)** | 0.062s (3.55x) |
| pi.lua | 0.377s (1.00x) | 0.575s (0.66x) | **0.009s (41.89x)** |
| regexdna.lua | 0.047s (1.00x) | 0.046s (1.02x) | **0.035s (1.34x)** |
| sieve.lua | 0.206s (1.00x) | 0.221s (0.93x) | **0.112s (1.84x)** |
| skynet.lua | 0.456s (1.00x) | **0.124s (3.68x)** | 0.243s (1.88x) |
| spectralnorm.lua | 0.385s (1.00x) | **0.016s (24.06x)** | 0.019s (20.26x) |
| warmup.lua | 0.006s (1.00x) | 0.005s (1.20x) | **0.005s (1.20x)** |

> Measured on an Apple A18 Pro · macOS 26.6.2 (ARM64) · Apple clang 21.0.0 · Average of 10 runs · clx `--fast`

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