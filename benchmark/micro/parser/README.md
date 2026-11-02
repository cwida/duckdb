# Parser microbenchmarks

The query benchmarks call `Parser::ParseQuery()` directly. A separate benchmark
measures compiled-grammar construction. They do not bind, optimize,
or execute SQL, and do not require tables, generated datasets, or optional extensions.
The realistic workloads read existing SQL files from the checkout before timing.
The implementation is in `benchmark/micro/parser.cpp`; the regression list is
`.github/regression/parser.csv`.

## Measurement boundary

Before timing, each query benchmark constructs or loads its SQL inputs, compiles the base grammar
once, and checks that valid input parses to the expected number of statements.
The deliberately malformed input is checked during the runs instead, so failed
parsing is covered by the runner's timeout.
The runner then performs one warmup batch and five timed batches by default.

Each timed batch repeats the same fixed inputs in order and includes:

- Construction of a fresh `Parser` using the shared compiled grammar.
- `ParseQuery`: tokenization, matching, and transformation into SQL statement ASTs.
- Statement-count checks and destruction of the parser and its results.

Grammar construction, query generation, file loading, database setup, and SQL execution are not
timed. This measures warm-grammar parsing, not first-query startup or just matching.
Parser options use their production defaults, including the choice of matcher.
There is no parsed-result cache shared between calls.

`ParserGrammarConstruction` is the exception: it times 250 independent calls to
`CompiledGrammar::Create()`, including destruction of each resulting grammar.
That includes reading/parsing the base grammar definition, constructing its keyword
helper/tables, and building its matchers. No compiled grammar is reused between
iterations, and no SQL query is parsed. The standard warmup still applies, so this
measures repeatable construction cost, not a cold process launch or cold filesystem.
Grammar extensions are not included in any of these benchmarks.

## Fixed workloads

| Benchmark | Input | Calls per timed batch | Statements per call |
|---|---|---:|---:|
| `ParserKeywordIdentifiers` | SELECT with keyword identifiers, filtering and ordering | 650 | 1 |
| `ParserWideSelect` | 128 arithmetic projection expressions and aliases | 55 | 1 |
| `ParserNestedExpressions` | 32 nested `coalesce` calls | 250 | 1 |
| `ParserMalformedSelect` | `select (((((((((((((;` (13 unmatched opening parentheses) | 400 | Expected syntax error |
| `ParserStatements` | 32 SELECT statements, quoted aliases and comments | 700 | 32 |
| `ParserTPCH` | All 22 TPC-H queries, repeated 12 times | 264 | 1 |
| `ParserTPCDS` | All 99 TPC-DS query files, parsed once | 99 | 1 |
| `ParserFlummi` | The approximately 373 KiB generated Flummi ray-tracing query, parsed once | 1 | 1 |
| `ParserAoC` | All 25 Advent of Code 2024 query files, repeated twice | 50 | 1 |
| `ParserStress` | One statement with an extreme instance of every expression shape the grammar treats differently | 7 | 1 |
| `ParserValuesList` | One large `VALUES` list | 8 | 1 |
| `ParserGrammarConstruction` | Construct and destroy the base grammar | 250 constructions | No SQL parsing |

The batch sizes target roughly 300 ms using representative median timings. Corpus workloads round
to a whole number of corpus repetitions, so their batch times can differ more from the target.

The small synthetic cases help isolate regressions. TPC-H and TPC-DS cover realistic
joins, subqueries, aggregation, CTEs and window functions; Flummi stresses parsing
one very large generated SQL statement with a recursive CTE.
AoC adds varied algorithmic SQL with recursive CTEs, string processing, nested queries
and embedded input strings. It measures parsing, not solving the puzzles.

SQL sources, loaded in numeric query order:

- TPC-H: `extension/tpch/dbgen/queries/q01.sql` through `q22.sql`.
- TPC-DS: `extension/tpcds/dsdgen/queries/01.sql` through `99.sql`.
- Stress: `benchmark/micro/parser/parser_stress.sql`.
- Values list: `benchmark/micro/parser/parser_values_list.sql`.
- Flummi: `benchmark/recursive_cte/queries/performance/flummi_ray.sql`.
- AoC: `benchmark/aoc24/queries/day01.sql` through `day25.sql`; provenance and license
  are documented in `benchmark/aoc24/README.md` and `benchmark/aoc24/LICENSE`.

Each file is a separate `ParseQuery` call, not one concatenated script. Missing,
empty or invalid files fail the benchmark rather than silently reducing the corpus.
TPC scale factors do not apply: only the SQL text is parsed, without generating data.

The runner reports **seconds per fixed batch**. To calculate microseconds per
`ParseQuery` call, multiply seconds by 1,000,000 and divide by the call count.
For `ParserStatements`, divide by another 32 for time per statement.
For `ParserGrammarConstruction`, divide batch seconds by 250 for seconds per
grammar construction/destruction cycle; do not interpret that result as SQL parsing time.

Keep the SQL files, input sizes and iteration counts unchanged when comparing revisions. If a
workload changes materially, give it a new benchmark name to preserve its history.
The count checks detect parse failures and missing statements; they are not a
replacement for parser correctness tests.

## What the targeted cases measure

### Nested expressions

`ParserNestedExpressions` starts with `value_column` and wraps it 32 times in
`coalesce(expression, i)`, for `i = 0` through `31`, then places it in a SELECT.
For example, three layers would be:

```sql
SELECT coalesce(coalesce(coalesce(value_column, 0), 1), 2)
FROM source_table
```

The actual benchmark uses 32 layers, not three. It exercises entering nested
expression rules, retaining parent matcher state, and building/destroying a nested
AST. `COALESCE` has a dedicated grammar rule, so this is not a benchmark of generic
function calls alone. It is a successful-parse depth test, not the malformed-input
backtracking test below. The expressions are never evaluated.

### Keyword identifiers

`ParserKeywordIdentifiers` parses this exact query:

```sql
SELECT abort, action, comment, database, first, last FROM source_table
WHERE action IS NOT NULL AND comment <> 'value' ORDER BY first, last
```

All six projected column names are built-in unreserved keywords. The parser must
recognize them as keywords while allowing them in identifier positions; `action`,
`comment`, `first` and `last` recur in other clauses. This measures the full parse of a
keyword-heavy query, not just one lookup operation, and does not cover every keyword
category. No table lookup, column binding or extension helper is involved.

### Malformed SELECT and packrat

`ParserMalformedSelect` parses the exact input `select (((((((((((((;` on every
iteration, with the normal production packrat behavior. Unfinished parentheses make
the parser explore failing expression alternatives; packrat memoization can avoid
repeating work for the same matcher at the same token position. Each parse call gets
fresh parsing state and its own packrat cache.

Every call must throw `ParserException`. Unexpected success fails verification, and
other exception types are not counted as expected syntax errors. Timing includes
tokenization, failed matching, error construction/exception handling, and cleanup;
this is not an isolated measurement of cache lookups or a packrat-on/off comparison.

## Build and run

From the repository root, build an optimized runner:

```sh
BUILD_BENCHMARK=1 make reldebug
```

Keep runtime artifacts outside the checkout. Make the existing SQL inputs visible
under the temporary root using symlinks (run this from the repository root):

```sh
parser_benchmark_tmp=$(mktemp -d /tmp/duckdb-parser-bench.XXXXXX)
ln -s "$PWD/benchmark" "$parser_benchmark_tmp/benchmark"
ln -s "$PWD/extension" "$parser_benchmark_tmp/extension"
build/reldebug/benchmark/benchmark_runner 'Parser.*' \
  --root-dir "$parser_benchmark_tmp" --timed-runs 10
```

Use `ParserNestedExpressions` instead of `Parser.*` to select one case. Add `--query` to
print its exact SQL, or `--info` for the fixed batch size. Query-execution profiling
does not apply to these parser-only benchmarks.
For a corpus, `--query` prints all of its input files in their parsing order.
`ParserGrammarConstruction` has no SQL input, so `--query` prints nothing for it.

## Compare revisions with the regression runner

Both binaries must contain these C++ benchmark registrations. Adding the CSV to
an old checkout alone is insufficient. For an older baseline, apply the same
benchmark harness and build-system addition there before building its runner;
keep the parser implementation under comparison unchanged.
The harness requires `CompiledGrammar` and `ParserOptions::compiled_grammar`;
older yacc-based revisions need an adapter that preserves the inputs and timed loop.

From the checkout containing the regression scripts:

```sh
python3 scripts/regression/test_runner.py \
  --old /path/to/base/build/reldebug/benchmark/benchmark_runner \
  --new /path/to/current/build/reldebug/benchmark/benchmark_runner \
  --benchmarks .github/regression/parser.csv \
  --samples 10 --threads 1
```

The regression script uses isolated temporary working directories and paired,
alternating samples. Both binaries read the same SQL files from the checkout where
the script is invoked. Use identical optimized build settings on both revisions,
the same machine and power mode, and avoid concurrent builds or other CPU-heavy
work. The parser workloads themselves are single-threaded.

Use clean build directories for comparisons. The regression script rejects a
build directory containing extension artifacts from multiple revisions.

The automatic regression workflow runs this list in its `Bench Parser` matrix job.
Both the baseline and current runner must contain the C++ benchmark registrations;
a baseline predating this harness will fail with a missing-benchmark error.
