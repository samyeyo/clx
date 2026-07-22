# Benchmarks

clx ships a suite of benchmarks under the `benchmarks/` directory. Each one
is a self-contained Lua script that can be run with plain Lua, LuaJIT, or as
a clx-compiled native binary so the three can be compared directly.

---

## Prerequisites

| Tool | Purpose |
|---|---|
| `lua` (5.5) | Reference interpreter |
| `luajit` (2.1) | JIT baseline |
| `clx` | Built from source — see [Getting Started](getting-started.md) |
| `hyperfine` (POSIX only) | Timing harness for the manual workflow |

> Windows users : you must create a lua\ folder in the root clx directory, that contains the lua55.exe / luajit.exe interpreters with their lua55.dll / lua51.dll libraries

---

## Running the full suite

### POSIX (Linux / macOS)

Run the automated script from the project root:

```sh
cd /path/to/clx
./benchmarks/run.sh
```

The script compiles every benchmark with clx, times all three engines over 10
runs (3 warmup), pins execution to a single CPU core when `taskset` is
available, and prints a formatted speedup table.

To pass extra flags to the C++ backend — for example to test a different
optimisation level:

```sh
CPPFLAGS="-O2" ./benchmarks/run.sh
```

A `hyperfine`-based variant is also provided:

```sh
./benchmarks/run-hyperfine.sh
```

### Windows

Run the automated script from a Developer Command Prompt with MSVC paths set.
A lua\ folder in the root clx directory must exists :  it should contain the lua55.exe / luajit.exe interpreters with their lua55.dll / lua51.dll libraries

```bat
cd \path\to\clx
benchmarks\run.bat
```

The script uses PowerShell's `System.Diagnostics.Stopwatch` for sub-millisecond
timing and forces `InvariantCulture` so decimal separators are correct on
European locales.  It performs 1 warmup run and 10 timed runs per benchmark.

## Benchmark Results

Performance improvements vs lua 5.5 (10 runs average, single CPU, hyperfine) :

| Script | lua 5.5 | LuaJIT | clx (speedup, `--fast`) |
|--------|---------|--------|--------------------------|
| 3ddist.lua | 0.092s (1.00x) | 0.018s (5.11x) | **0.015s (6.13x)** |
| ackermann.lua | 0.051s (1.00x) | 0.011s (4.64x) | **0.008s (6.37x)** |
| arraysum.lua | 0.128s (1.00x) | 0.052s (2.46x) | **0.031s (4.13x)** |
| binarytrees.lua | 0.020s (1.00x) | **0.013s (1.54x)** | 0.024s (0.83x) |
| bubble.lua | 0.051s (1.00x) | **0.007s (7.29x)** | 0.014s (3.64x) |
| canada.lua | 0.372s (1.00x) | **0.142s (2.62x)** | 0.286s (1.30x) |
| coro.lua | 0.005s (1.00x) | 0.007s (0.71x) | 0.006s (0.83x) |
| fannkuchredux.lua | 0.010s (1.00x) | **0.007s (1.43x)** | 0.007s (1.43x) |
| fasta.lua | 0.007s (1.00x) | 0.006s (1.17x) | **0.005s (1.40x)** |
| fib.lua | 0.311s (1.00x) | 0.045s (6.91x) | **0.005s (62.20x)** |
| hashtable.lua | 0.942s (1.00x) | **0.359s (2.62x)** | 0.596s (1.58x) |
| json.lua | 0.155s (1.00x) | 1.027s (0.15x) | **0.021s (7.38x)** |
| knucleotide.lua | 0.014s (1.00x) | 0.014s (1.00x) | **0.009s (1.56x)** |
| life.lua | 0.075s (1.00x) | **0.018s (4.17x)** | 0.019s (3.95x) |
| mandelbrot.lua | 0.012s (1.00x) | 0.007s (1.71x) | **0.004s (3.00x)** |
| nbody.lua | 0.009s (1.00x) | **0.007s (1.29x)** | 0.009s (1.00x) |
| pi.lua | 0.079s (1.00x) | 0.040s (1.98x) | **0.021s (3.76x)** |
| sieve.lua | 0.083s (1.00x) | 0.040s (2.08x) | **0.039s (2.13x)** |
| spectralnorm.lua | 0.310s (1.00x) | **0.018s (17.22x)** | 0.029s (10.69x) |
| warmup.lua | 0.006s (1.00x) | 0.005s (1.20x) | **0.005s (1.20x)** |

> Measured on Intel® Core™ i5 Ultra 125U CPU @ 4.30GHz · Linux · GCC 13.3.0

---

## Benchmark descriptions

### `fib.lua` — recursive Fibonacci
Classic tree-recursive `fib(n)` with no memoisation (O(2^n) complexity).
`fib(34)` computes 5702887.  Stresses function-call overhead and arithmetic performance.  clx eliminates boxing for the integer return value and emits direct native arithmetic.

### `ackermann.lua` — Ackermann function
Deeply recursive function with no numeric regularity that prevents loop
optimisation.  Primarily a measure of call overhead and stack performance.

### `spectralnorm.lua` — spectral norm  *(Computer Language Benchmarks Game)*
Computes the spectral norm of an infinite matrix using the power method.
Dense floating-point inner loops with repeated array accesses; a good measure
of how well the compiler can infer numeric types and eliminate value boxing.

### `nbody.lua` — N-body simulation  *(Computer Language Benchmarks Game)*
Simulates a small planetary system using Newtonian gravity. Heavy
floating-point arithmetic on a fixed set of named fields; exercises struct-like
table access and numeric type propagation across function boundaries.

### `mandelbrot.lua` — Mandelbrot set  *(Computer Language Benchmarks Game)*
Iterates the Mandelbrot recurrence over a 2D grid and writes a PBM bitmap to
stdout.  Inner loop mixes complex-number arithmetic with early-exit logic;
stresses loop optimisation and float-to-int conversion.

### `fannkuchredux.lua` — Fannkuch-Redux  *(Computer Language Benchmarks Game)*
Counts permutation flips according to the Pancake sorting problem.  Array-heavy
with tight indexed loops; measures raw table-access and integer arithmetic
performance.

### `binarytrees.lua` — binary trees  *(Computer Language Benchmarks Game)*
Allocates, traverses, and deallocates millions of two-field tree nodes.
Primarily a GC stress test; the workload is dominated by allocation rate and
collection pause time rather than arithmetic.

### `knucleotide.lua` — k-nucleotide  *(Computer Language Benchmarks Game)*
Counts k-mer frequencies in a DNA strand using Lua tables as hash maps.
Stresses string hashing, table insertion, and iteration.

### `fasta.lua` — Fasta  *(Computer Language Benchmarks Game)*
Generates pseudo-random DNA sequences according to weighted probability tables
and writes them in FASTA format.  Mixes integer arithmetic, string building,
and I/O.

### `bubble.lua` — bubble sort
Sorts a worst-case (descending) integer array of 8 000 elements using plain
bubble sort.  The O(n²) loop body is minimal — almost all time is in indexed
table reads/writes and integer comparisons, making it a direct measure of array
throughput.

### `arraysum.lua` — array sum
Iterates over a five-million-element numeric array and sums its values.
Minimal logic; measures loop overhead and numeric array read performance.

### `hashtable.lua` — hash table
Inserts 100 000 string-keyed entries into a Lua table, then reads them all
back.  Measures string interning, table hashing, and the cost of mixed
key/value boxing.

### `pi.lua` — Monte Carlo π
Estimates π by testing whether random points fall inside the unit circle, using
a deterministic LCG rather than `math.random` so results are reproducible.
Pure integer and float arithmetic in a tight loop.

### `3ddist.lua` — 3D Euclidean distance
Computes two million `math.sqrt` calls on deterministically generated 3D
coordinates.  Measures the cost of the `math` library dispatch and float
arithmetic; a good proxy for scientific or simulation inner loops.

### `life.lua` — Conway's Game of Life
Simulates 300 generations of Life on a 40×20 grid using nested table access.
Exercises two-dimensional array traversal and integer conditionals in a
moderately complex, non-trivial control flow pattern.

### `sieve.lua` — Sieve of Eratosthenes
Finds all primes up to 1 000 000 using a boolean array sieve.  Dense
indexed-write loop; measures integer array write throughput and branch
prediction behaviour.

### `json.lua` — JSON encoder
Encodes 8 000 heterogeneous Lua tables to a JSON-like string using a recursive
encoder.  Stresses string concatenation, `type()` dispatch, and recursive
function calls on mixed-type data.

### `coro.lua` — coroutine yield
Runs 5 million resume/yield cycles between a producer and consumer, summing the yielded values (expected result: 12500002500000).  Measures raw coroutine creation and resume/yield overhead at scale.

### `canada.lua` — GeoJSON parsing  *(real-world workload)*
Parses the canonical 2.2 MB `canada.json` file (the Canada GeoJSON polygon
dataset) using [dkjson](http://dkolf.de/src/dkjson-lua.fsl/home), then walks
every coordinate pair across all features to compute the dataset's geographic
bounding box.

This is the most representative real-world benchmark in the suite.  Unlike the
synthetic benchmarks it exercises the full pipeline end-to-end: file I/O,
large string handling, a non-trivial third-party Lua library, deep table
traversal, and mixed string/number/table workloads — the kind of program a Lua
developer would actually write.

**Required files** — both must be present in the working directory:

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

### `warmup.lua` — startup latency
Minimal script that performs a simple allocation and exits.  Designed to
measure the fixed overhead between runtime initialisation and effective
program execution — the time a clx-compiled binary takes to load the runtime,
initialise the Lua state, and reach the first application statement.  Because
the benchmark body itself is negligible, any difference between engines is
entirely attributable to startup cost rather than execution speed.

---
