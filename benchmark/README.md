
#### Clone the repo and compile the benchmark runner

```
git clone https://github.com/duckdb/duckdb
cd duckdb
BUILD_BENCHMARK=1 BUILD_TPCH=1 make
```

#### List all available benchmarks
`build/release/benchmark/benchmark_runner --list`

#### Run a single benchmark
`build/release/benchmark/benchmark_runner benchmark/micro/nulls/no_nulls_addition.benchmark`

The output will be printed to `stderr` in CSV format, in the following format:

```
name	run	timing
benchmark/micro/nulls/no_nulls_addition.benchmark	1	0.121234
benchmark/micro/nulls/no_nulls_addition.benchmark	2	0.121702
benchmark/micro/nulls/no_nulls_addition.benchmark	3	0.122948
benchmark/micro/nulls/no_nulls_addition.benchmark	4	0.122534
benchmark/micro/nulls/no_nulls_addition.benchmark	5	0.124102
```

By default, every benchmark emits five timed runs after one warmup run. You can override this using `--timed-runs n`:

```
build/release/benchmark/benchmark_runner benchmark/micro/nulls/no_nulls_addition.benchmark --timed-runs 20
```

You can also specify an output file using the `--out` flag. This will write only the timings (delimited by newlines) to that file.

```
build/release/benchmark/benchmark_runner benchmark/micro/nulls/no_nulls_addition.benchmark --out=timings.out
cat timings.out
0.182472
0.185027
0.184163
0.185281
0.182948
```

#### Regex
You can also use a regex to specify which benchmarks to run. Be careful of shell expansion of certain regex characters (e.g. `*` will likely be expanded by your shell, hence this requires proper quoting or escaping).

`build/release/benchmark/benchmark_runner "benchmark/micro/nulls/.*" `

#### Run all benchmarks
Not specifying any argument will run all benchmarks.

`build/release/benchmark/benchmark_runner`

#### Parser-only microbenchmarks

Use the `Parser.*` benchmarks to measure `Parser::ParseQuery()` without query execution.
See [the parser benchmark guide](micro/parser/README.md) for workloads, timing boundaries,
and revision comparisons using `.github/regression/parser.csv`.

#### Other options
`--info` gives you some other information about the benchmark.

```
build/release/benchmark/benchmark_runner benchmark/micro/nulls/no_nulls_addition.benchmark --info
display_name:NULL Addition (no nulls)
group:micro
subgroup:nulls
```

`--query` will print the query that is run by the benchmark.

```
SELECT MIN(i + 1) FROM integers
```

`--profile` will output a query tree (pretty printed), primarily intended for interactive use.

```
┌─────────────────────────────────────┐
│┌───────────────────────────────────┐│
││    Query Profiling Information    ││
│└───────────────────────────────────┘│
└─────────────────────────────────────┘
SELECT MIN(i + 1) FROM integers
┌─────────────────────────────────────┐
│┌───────────────────────────────────┐│
││         Total Time: 0.176s        ││
│└───────────────────────────────────┘│
└─────────────────────────────────────┘
┌───────────────────────────┐
│    UNGROUPED_AGGREGATE    │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│          min(#0)          │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│             1             │
│          (0.03s)          │
└─────────────┬─────────────┘                             
┌─────────────┴─────────────┐
│         PROJECTION        │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│          +(i, 1)          │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│         100000000         │
│          (0.05s)          │
└─────────────┬─────────────┘                             
┌─────────────┴─────────────┐
│          SEQ_SCAN         │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│          integers         │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│             i             │
│   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │
│         100000000         │
│          (0.08s)          │
└───────────────────────────┘      
```

