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

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Linux · GCC 15.2.0 · Average of 10 runs · clx `--fast` built with AVX2

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

### Windows x86-64 (MSVC cl.exe)

Speedups vs. Lua 5.5 (10-run average, `benchmarks\run.bat`):

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.340s (1.00x) | 0.084s (4.06x) | **0.065s (5.21x)** |
| ackermann.lua | 0.203s (1.00x) | **0.066s (3.08x)** | 0.100s (2.02x) |
| arraysum.lua | 0.274s (1.00x) | 0.132s (2.07x) | **0.119s (2.31x)** |
| binarytrees.lua | 0.515s (1.00x) | **0.184s (2.81x)** | 0.283s (1.82x) |
| bubble.lua | 0.298s (1.00x) | **0.049s (6.08x)** | 0.161s (1.85x) |
| canada.lua | 0.287s (1.00x) | **0.132s (2.17x)** | 0.297s (0.97x) |
| coro.lua | 7.113s (1.00x) | **0.215s (33.04x)** | 0.730s (9.75x) |
| fannkuchredux.lua | 1.853s (1.00x) | **0.417s (4.44x)** | 1.898s (0.98x) |
| fasta.lua | 1.577s (1.00x) | 1.340s (1.18x) | **0.621s (2.54x)** |
| fib.lua | 0.309s (1.00x) | 0.098s (3.14x) | **0.077s (4.01x)** |
| hashtable.lua | 0.976s (1.00x) | **0.330s (2.96x)** | 0.427s (2.29x) |
| json.lua | 0.721s (1.00x) | 1.969s (0.37x) | **0.065s (11.14x)** |
| knucleotide.lua | 0.143s (1.00x) | **0.090s (1.59x)** | 0.124s (1.16x) |
| life.lua | 0.301s (1.00x) | **0.099s (3.03x)** | 0.220s (1.37x) |
| mandelbrot.lua | 0.243s (1.00x) | 0.094s (2.59x) | **0.071s (3.42x)** |
| nbody.lua | 0.184s (1.00x) | **0.052s (3.56x)** | 0.110s (1.67x) |
| pi.lua | 0.318s (1.00x) | 0.191s (1.67x) | **0.077s (4.12x)** |
| regexdna.lua | 0.085s (1.00x) | 0.087s (0.98x) | **0.080s (1.06x)** |
| sieve.lua | 0.327s (1.00x) | **0.188s (1.74x)** | 0.217s (1.50x) |
| skynet.lua | 1.341s (1.00x) | **0.205s (6.55x)** | 0.440s (3.05x) |
| spectralnorm.lua | 0.323s (1.00x) | **0.052s (6.17x)** | 0.057s (5.70x) |
| warmup.lua | 0.040s (1.00x) | **0.040s (0.98x)** | 0.041s (0.95x) |

> Measured on an Intel® Core™ Ultra 5 125U CPU @ 4.30GHz · Windows 11 · MSVC 19.50.35724 · Average of 10 runs · clx `--fast` built with AVX2

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