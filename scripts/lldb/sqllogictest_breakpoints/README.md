# LLDB Helpers

This directory contains small LLDB helpers for DuckDB development.

## `sql_break.py`

Adds `sql_`-prefixed LLDB commands for sqllogictest-aware debugging.

It assumes the active sqllogictest is running on thread 1 and uses
`query_break` as the hook before each sqllogictest statement/query.

### One-off usage

```lldb
command script import <duckdb repository root>/scripts/lldb/sqllogictest_breakpoints/sql_break.py
b <some breakpoint>
r
sql_current_statement
sql_current_statement --json
sql_next_statement
sql_next_matching_statement --sql "JOIN"
sql_next_matching_statement --kind query
sql_next_matching_statement --kind statement_ok
sql_next_matching_statement --kind statement_error
sql_next_matching_statement --connection con2
sql_watch_statement --file test/sql/join --loop i=3
sql_watch_statement --connection con2
sql_next_watch
sql_list_watches
sql_delete_watch 5
sql_delete_watch
sql_delete_watch all
```

### Suggested `~/.lldbinit`

```lldb
command script import <duckdb repository root>/scripts/lldb/sqllogictest_breakpoints/sql_break.py
```

### Commands

- `sql_current_statement`
  - prints the sqllogictest file, line, kind, connection, SQL, and active loop values
  - `--json` returns the same context as a JSON object for programmatic inspection
- `sql_next_statement`
  - continues until the next sqllogictest statement/query
- `sql_next_matching_statement`
  - continues until the next sqllogictest statement/query matching the supplied filters
- `sql_watch_statement`
  - installs a persistent sqllogictest-aware watch rule
- `sql_next_watch`
  - continues until one installed watch rule matches
- `sql_list_watches`
  - lists installed watch rules
- `sql_delete_watch <id>`
  - removes one installed watch rule, the most recently added watch if omitted, or all watches with `all`

### Filters

`sql_next_matching_statement` and `sql_watch_statement` support:

- `--file <substring>`
- `--line <n>`
- `--line-min <n>`
- `--line-max <n>`
- `--kind query|statement|statement_ok|statement_error`
- `--connection <name>`
- `--sql <substring>` (case-sensitive match against the expanded SQL)
- `--loop <name>`
- `--loop <name>=<value>`

Filters combine with AND. Quote arguments containing spaces. The navigation commands
skip the runner's duplicate hook for a single `statement`, so advancing moves to the
next statement/query.

### JSON Context

After stopping, use `sql_current_statement --json` to retrieve a structured snapshot:

```json
{
  "kind": "query",
  "statement_expectation": null,
  "file_name": "test/example.test",
  "query_line": 23,
  "sql_text": "SELECT 2 - 2 AS loop_value",
  "connection_name": "con2",
  "running_loops": [{"name": "i", "value": "2"}],
  "loop_values": {"i": "2"}
}
```

Unavailable fields are `null`; no active loops yields an empty array and object.
`query_line` is the runner's one-based directive line. Loop values are strings.
Use `sql_next_matching_statement --sql "AS loop_value"` to find SQL without a fixed
line number, then retrieve JSON at the resulting stop. If driving LLDB through its
Python API, read the output and success status from `SBCommandReturnObject`.

### Watch Behavior

`sql_watch_statement` defines persistent watch rules, but normal `c` does not stop on
those rules by itself.

Use `sql_next_watch` to temporarily arm the installed watch rules while
auto-continuing your other breakpoints until the first watch match is reached.

You can optionally pass a watch id to only arm one installed watch:

```lldb
sql_next_watch
sql_next_watch 5
```
