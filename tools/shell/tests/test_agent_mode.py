# fmt: off

import re
import pytest
from conftest import ShellTest

# Agent mode renders output for an AI coding agent that reads it through a pipe: a compact markdown table (no
# padding, the type in the header cell) rendered whole up to 1000 rows / 10000 bytes and as a first-and-last-rows
# sample beyond that, cells cut at 500 chars, a footer with the row count and a hash of the whole result, JSON
# errors, compact plans, a one-line startup note and estimate/progress lines on stderr. It is detected from the environment variables coding agents set for the
# commands they run, and can be forced with -agent / -no-agent.

FIFTY_ROWS = "SELECT range AS r FROM range(50)"

def agent_shell(shell, agent_var="AI_AGENT", agent_value="test-agent"):
    return ShellTest(shell).env_var(agent_var, agent_value)

def test_default_is_unchanged(shell):
    # without an agent in the environment, nothing changes: unicode borders, 40-row limit
    test = ShellTest(shell).statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("┌")
    result.check_stdout("50 rows")
    result.check_stdout("40 shown")

@pytest.mark.parametrize("agent_var,agent_value", [
    ("AI_AGENT", "claude-code_2-1-282_agent"),
    ("AGENT", "amp"),
    ("CLAUDECODE", "1"),
    ("CODEX_CI", "1"),
    ("CURSOR_AGENT", "1"),
    ("GEMINI_CLI", "1"),
    ("COPILOT_AGENT", "1"),
])
def test_detect_agent(shell, agent_var, agent_value):
    test = agent_shell(shell, agent_var, agent_value).statement(FIFTY_ROWS)
    result = test.run()
    # compact markdown, types in the header, all 50 rows (under the cap)
    result.check_stdout("| r:BIGINT |\n|---|\n| 0 |")
    result.check_stdout("| 49 |\n50 rows")
    result.check_not_exist("shown")
    result.check_not_exist("┌")

def test_empty_variable_is_not_an_agent(shell):
    test = agent_shell(shell, "AI_AGENT", "").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("┌")
    result.check_stdout("40 shown")

def test_compact_table(shell):
    # no alignment padding, the type in the header cell, no row count for a handful of rows
    test = agent_shell(shell).statement("SELECT 42 AS a, 'hello' AS b, NULL AS c, 'x|y' AS d")
    result = test.run()
    result.check_stdout("""| a:INTEGER | b:VARCHAR | c:NULL | d:VARCHAR |
|---|---|---|---|
| 42 | hello | NULL | x\\|y |""")
    result.check_not_exist("row")

def test_row_count_footer(shell):
    # the row count is rendered from 10 rows on, where counting is error-prone
    test = agent_shell(shell).statement("SELECT range AS r FROM range(9)").statement("SELECT range AS r FROM range(10)")
    result = test.run()
    result.check_stdout("| 8 |\n| r:BIGINT |")
    result.check_stdout("| 9 |\n10 rows")

def test_nested_types_in_header(shell):
    test = agent_shell(shell).statement("SELECT sum(i) AS total, list(i) AS l, {'x': max(i)} AS s FROM range(3) t(i)")
    result = test.run()
    result.check_stdout("| total:HUGEINT | l:BIGINT[] | s:STRUCT(x BIGINT) |")

def test_empty_result(shell):
    test = agent_shell(shell).statement("SELECT 42 AS a WHERE false")
    result = test.run()
    result.check_stdout("| a:INTEGER |\n|---|\n0 rows")

def test_no_agent_flag(shell):
    test = agent_shell(shell).add_argument("-no-agent").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("┌")
    result.check_stdout("40 shown")

def test_agent_flag(shell):
    # no agent in the environment, but forced on
    test = ShellTest(shell).add_argument("-agent").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("| r:BIGINT |")
    result.check_stdout("| 49 |\n50 rows")
    result.check_not_exist("shown")

def test_output_mode_flag_turns_agent_mode_off(shell):
    # an output mode on the command line is a deliberate choice of format: detected agent mode stays off entirely,
    # so no preamble, no JSON errors, no estimate lines
    test = agent_shell(shell).add_argument("-csv").statement("SELECT 42 AS a").statement(".show")
    result = test.run()
    result.check_stdout("a\n42")
    result.check_stdout("agent: off")
    result.check_not_exist("|")
    assert "agent mode" not in result.stderr
    assert "estimate:" not in result.stderr

def test_output_mode_flag_plain_errors(shell):
    test = agent_shell(shell).add_argument("-csv").statement("SELECT foo FROM (SELECT 1 AS bar)")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr("LINE 1:")
    assert "exception_type" not in result.stderr
    # the agent chose its format itself - no exit hint either
    assert "hint:" not in result.stderr

def test_agent_flag_with_output_mode(shell):
    # -agent is explicit too: it keeps the mode on, only the format changes
    test = ShellTest(shell).add_argument("-agent", "-csv").statement("SELECT 42 AS a").statement(".show")
    result = test.run()
    result.check_stdout("a\n42")
    result.check_stdout("agent: on")
    result.check_stderr("duckdb agent mode on (-agent)")

def test_duckbox_all_rows(shell):
    # switching back to duckbox keeps the agent-mode row cap (1000), so all 50 rows show
    test = agent_shell(shell).statement(".mode duckbox").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("│    49 │")
    result.check_not_exist("shown")

def test_maxrows_wins(shell):
    test = agent_shell(shell).statement(".mode duckbox").statement(".maxrows 10").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("10 shown")

def test_show(shell):
    test = agent_shell(shell, "AI_AGENT", "some-agent").statement(".show")
    result = test.run()
    result.check_stdout("agent: some-agent")

def test_show_marker_name(shell):
    test = agent_shell(shell, "CLAUDECODE", "1").statement(".show")
    result = test.run()
    result.check_stdout("agent: claude-code")

def test_json_error(shell):
    test = agent_shell(shell).statement("SELECT foo FROM (SELECT 1 AS bar)")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr('"exception_type":"Binder"')
    result.check_stderr('"exception_message":"Referenced column \\"foo\\" not found in FROM clause!')
    result.check_stderr('"position":"7"')
    # the LINE/caret block is gone, and so is the candidates field (the message already lists them)
    assert "LINE 1" not in result.stderr
    assert "^^^" not in result.stderr
    assert '"candidates"' not in result.stderr

def test_json_parser_error(shell):
    test = agent_shell(shell).statement("SELEC 1")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr('"exception_type":"Parser"')

def test_json_shell_error(shell):
    # errors raised by the shell itself (not the engine) are JSON too
    test = agent_shell(shell).statement(".nonsense")
    result = test.run()
    result.check_stderr('"exception_type":"Invalid Input"')
    result.check_stderr('"exception_message":"Unknown Command Error: Unrecognized command \'nonsense\'')

def test_error_without_agent(shell):
    test = ShellTest(shell).statement("SELECT foo FROM (SELECT 1 AS bar)")
    result = test.run()
    result.check_stderr("LINE 1:")
    assert "exception_type" not in result.stderr

def test_compact_explain(shell):
    test = agent_shell(shell).statement("EXPLAIN SELECT count(*) FROM range(10) r JOIN range(5) s ON r.range = s.range")
    result = test.run()
    result.check_stdout("UNGROUPED_AGGREGATE (est=")
    result.check_stdout("\n  HASH_JOIN (est=")
    result.check_stdout("Join Type: INNER; Conditions:")
    result.check_not_exist("╭")

def test_compact_explain_analyze(shell):
    test = agent_shell(shell).statement("EXPLAIN ANALYZE SELECT count(*) FROM range(10)")
    result = test.run()
    result.check_stdout("QUERY (time=")
    result.check_stdout("UNGROUPED_AGGREGATE (est=")
    result.check_stdout(", rows=1, time=")

def test_compact_explain_analyze_without_operators(shell):
    # a count(*) over a table is answered from metadata: no operator tree, but still a summary line
    test = (
        agent_shell(shell)
        .statement("CREATE TABLE t AS SELECT range AS i FROM range(10)")
        .statement("EXPLAIN ANALYZE SELECT count(*) FROM t")
    )
    result = test.run()
    result.check_stdout("QUERY (time=")

def test_explicit_explain_format_wins(shell):
    test = agent_shell(shell).statement("EXPLAIN (FORMAT json) SELECT 42")
    result = test.run()
    result.check_stdout('"name":')
    result.check_not_exist("(est=")

def test_progress_on_stderr(shell):
    # progress lines are printed to stderr; progress_bar_time=0 makes them appear immediately
    test = (
        agent_shell(shell)
        .statement("SET progress_bar_time=0")
        .statement("SELECT count(*) FROM range(50000000)")
    )
    result = test.run()
    result.check_stdout("50000000")
    result.check_stderr("progress: ")
    result.check_stderr("% (elapsed ")
    result.check_stderr("progress: done (elapsed ")

def test_no_progress_without_agent(shell):
    test = (
        ShellTest(shell)
        .statement("SET progress_bar_time=0")
        .statement("SELECT count(*) FROM range(50000000)")
    )
    result = test.run()
    result.check_stdout("50000000")
    assert "progress:" not in result.stderr

def test_estimate_on_stderr(shell):
    # before a query that reads a lot runs, the planner's estimate of what it reads and returns goes to stderr
    test = (
        agent_shell(shell)
        .statement("CREATE TABLE t AS SELECT range AS i FROM range(2000000)")
        .statement("SELECT count(*) FROM t JOIN range(10) r ON t.i = r.range")
    )
    result = test.run()
    result.check_stdout("| count_star():BIGINT |\n|---|\n| 10 |")
    # no "rows returned" for a statement that does not return rows
    result.check_stderr("estimate: ~2000000 rows read (range ~2000000)\n")
    result.check_stderr("estimate: ~2000010 rows read (t ~2000000, range ~10), ~1 rows returned")

def test_no_estimate_for_small_reads(shell):
    # the line is there to decide whether to wait; a small read is noise
    test = agent_shell(shell).statement("SELECT count(*) FROM range(100000)")
    result = test.run()
    result.check_stdout("100000")
    assert "estimate:" not in result.stderr

def test_no_estimate_without_scans(shell):
    test = (
        agent_shell(shell)
        .statement("CREATE TABLE t(i INTEGER)")
        .statement("SET threads=2")
        .statement("SELECT 42")
    )
    result = test.run()
    result.check_stdout("42")
    assert "estimate:" not in result.stderr

def test_no_estimate_without_agent(shell):
    test = ShellTest(shell).statement("SELECT count(*) FROM range(1000)")
    result = test.run()
    assert "estimate:" not in result.stderr

def test_echo_second_statement(shell):
    # the statement text the shell works with must be each statement's own text
    test = ShellTest(shell).add_argument("-echo", "-c", "SELECT 1 AS a; SELECT 2 AS b")
    result = test.run()
    result.check_stdout("SELECT 2 AS b")


def test_tables_compact(shell):
    # .tables lists one line per table instead of the box layout
    test = (
        agent_shell(shell)
        .statement("CREATE TABLE t1(a INTEGER PRIMARY KEY, b VARCHAR)")
        .statement("CREATE VIEW v1 AS SELECT a FROM t1")
        .statement(".tables")
    )
    result = test.run()
    result.check_stdout("memory.main.t1 (table, ~0 rows): a INTEGER PK, b VARCHAR")
    result.check_stdout("memory.main.v1 (view): a INTEGER")
    result.check_not_exist("\x1b[")

def test_preamble_on_stderr(shell):
    # before anything runs: that the mode switched itself on and how to undo that, what the output means, and the
    # knobs the reader would not know about
    test = agent_shell(shell).statement("SELECT 1 AS a")
    result = test.run()
    result.check_stderr(
        "duckdb agent mode on: AI_AGENT is set and stdout is not a terminal; -no-agent turns it off, "
        ".help agent explains the output"
    )
    # one line only - the explanation is behind .help agent
    assert "output:" not in result.stderr
    assert "tips:" not in result.stderr
    result.check_stdout("| a:INTEGER |\n|---|\n| 1 |")
    assert "agent mode" not in result.stdout

def test_help_agent(shell):
    test = agent_shell(shell).statement(".help agent")
    result = test.run()
    result.check_stdout("agent mode: test-agent")
    result.check_stdout("output: markdown tables; a result of up to 1000 rows and 10000 bytes is rendered whole")
    result.check_stdout("tips: SET max_execution_time=<ms>")
    result.check_stdout("switches: -agent / -no-agent")

def test_help_agent_without_agent(shell):
    # the defaults, even though they are not in effect
    test = ShellTest(shell).statement(".help agent")
    result = test.run()
    result.check_stdout("agent mode: off")
    result.check_stdout("up to 1000 rows and 10000 bytes")

def test_preamble_names_the_marker(shell):
    test = agent_shell(shell, "CLAUDECODE", "1").statement("SELECT 1 AS a")
    result = test.run()
    result.check_stderr("duckdb agent mode on: CLAUDECODE is set and stdout is not a terminal;")

def test_preamble_when_forced(shell):
    # nothing to undo when the flag asked for it
    test = ShellTest(shell).add_argument("-agent").statement("SELECT 1 AS a")
    result = test.run()
    result.check_stderr("duckdb agent mode on (-agent); .help agent explains the output")
    assert "-no-agent" not in result.stderr

def test_no_preamble_without_agent(shell):
    test = ShellTest(shell).statement("SELECT 1 AS a")
    result = test.run()
    assert "agent mode" not in result.stderr

def test_preamble_off_via_init_file(shell, tmp_path):
    # the preamble follows the init file, so ~/.duckdbrc can silence it and set the caps
    init = tmp_path / "duckdbrc"
    init.write_text(".startup_text none\n.maxrows 5\n")
    test = ShellTest(shell, ["-init", str(init)]).env_var("AI_AGENT", "test-agent").statement("SELECT range AS r FROM range(20)")
    result = test.run()
    assert "agent mode" not in result.stderr
    result.check_stdout(
        "| 2 |\n| … 15 rows omitted … |\n| 18 |\n| 19 |\nfirst 3 and last 2 of 20 rows (.maxrows -1 for all), hash "
    )

def test_row_cap_is_loud(shell):
    # a result beyond the cap is a sample of its first and last rows, with an explicit marker row in between
    test = agent_shell(shell).statement("SELECT range AS r FROM range(2500)")
    result = test.run()
    result.check_stdout("| 19 |\n| … 2460 rows omitted … |\n| 2480 |")
    result.check_stdout("| 2499 |\nfirst 20 and last 20 of 2500 rows (.maxrows -1 for all), hash ")
    assert "| 20 |" not in result.stdout

def test_byte_cap_is_loud(shell):
    # wide rows: each side of the sample gets half the byte budget
    test = agent_shell(shell).statement("SELECT repeat('x', 400) AS s, range AS r FROM range(2500)")
    result = test.run()
    result.check_stdout("| … 2476 rows omitted … |")
    result.check_stdout("first 12 and last 12 of 2500 rows (.maxbytes 0 for all), hash ")

def test_result_within_cap_is_whole(shell):
    test = agent_shell(shell).statement("SELECT range AS r FROM range(1000)")
    result = test.run()
    result.check_stdout("| 999 |\n1000 rows, hash ")
    assert "omitted" not in result.stdout

def test_first_row_always_renders(shell):
    # a budget below one row is no reason to show nothing
    test = agent_shell(shell).statement(".maxbytes 10").statement("SELECT repeat('x', 50) AS s FROM range(3)")
    result = test.run()
    result.check_stdout("| " + "x" * 50 + " |\nfirst 1 of 3 rows (.maxbytes 0 for all), hash ")

def test_caps_lifted(shell):
    test = (
        agent_shell(shell)
        .statement(".maxrows -1")
        .statement(".maxbytes 0")
        .statement("SELECT range AS r FROM range(1200)")
    )
    result = test.run()
    result.check_stdout("| 1199 |\n1200 rows, hash ")

def test_huge_result_stops_early(shell):
    # beyond the cap the rows are only counted (and hashed) for a while, then the query is stopped: the count is a
    # lower bound and there is no hash of a result that was not read to the end
    test = agent_shell(shell).statement("SELECT range AS r FROM range(1000000000)")
    result = test.run()
    result.check_stdout("| 19 |\nfirst 20 of at least ")
    result.check_stdout(" rows (query stopped early; .maxrows -1 for all)")
    assert "hash" not in result.stdout
    assert "omitted" not in result.stdout

def test_hash_is_order_independent(shell):
    test = (
        agent_shell(shell)
        .statement("SELECT range AS r FROM range(20) ORDER BY r")
        .statement("SELECT range AS r FROM range(20) ORDER BY r DESC")
        .statement("SELECT range AS r FROM range(20) WHERE r <> 5")
        .statement("SELECT range AS r FROM range(20) UNION ALL SELECT 5")
    )
    result = test.run()
    hashes = re.findall(r"hash ([0-9a-f]{16})", result.stdout)
    assert len(hashes) == 4
    assert hashes[0] == hashes[1]
    assert hashes[2] != hashes[0]
    assert hashes[3] != hashes[0]

def test_hash_covers_rows_beyond_cap(shell):
    test = (
        agent_shell(shell)
        .statement(".maxrows 5")
        .statement("SELECT range AS r FROM range(20) ORDER BY r")
        .statement("SELECT range AS r FROM range(20) ORDER BY r DESC")
    )
    result = test.run()
    hashes = re.findall(r"first 3 and last 2 of 20 rows \(\.maxrows -1 for all\), hash ([0-9a-f]{16})", result.stdout)
    assert len(hashes) == 2
    assert hashes[0] == hashes[1]

def test_hash_column_order_and_null(shell):
    test = (
        agent_shell(shell)
        .statement("SELECT range AS a, range + 1 AS b FROM range(10)")
        .statement("SELECT range + 1 AS b, range AS a FROM range(10)")
        .statement("SELECT NULL AS x FROM range(10)")
        .statement("SELECT 'NULL' AS x FROM range(10)")
    )
    result = test.run()
    hashes = re.findall(r"hash ([0-9a-f]{16})", result.stdout)
    assert len(hashes) == 4
    assert len(set(hashes)) == 4

def test_cell_cut_is_loud(shell):
    # cut at 500 characters (not bytes), pipes still escaped, and the cut is marked
    test = agent_shell(shell).statement("SELECT repeat('é|', 300) AS s, 'short' AS t")
    result = test.run()
    result.check_stdout("é\\|…(+100 chars) | short |")

def test_maxcellwidth(shell):
    test = agent_shell(shell).statement(".maxcellwidth 0").statement("SELECT repeat('x', 600) AS s")
    result = test.run()
    result.check_stdout("x" * 600)
    assert "chars)" not in result.stdout

def test_timeout_is_a_json_error(shell):
    test = (
        agent_shell(shell)
        .statement("SET max_execution_time=200")
        .statement("SELECT count(*) FROM range(3000000000) t1, range(100) t2 WHERE (t1.range * t2.range) % 7 = 3")
    )
    result = test.run()
    assert result.status_code == 1
    result.check_stderr('"exception_type":"INTERRUPT"')
    result.check_stderr("Query exceeded maximum execution time")

def test_streamed_error_is_reported(shell):
    # an error raised while the result streams (after the header went out) is reported, not swallowed
    test = agent_shell(shell).statement("SELECT (1 / (r - 5))::INTEGER AS x FROM range(10) t(r)")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr('"exception_type":"Conversion"')
    assert "rows" not in result.stdout

def test_exit_hint_after_failure(shell):
    # a failed run through a pipe, with no agent detected and neither flag given, may be an agent that does not know
    # the mode exists
    test = ShellTest(shell).statement("SELEC 1")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr(
        "\nhint: -agent renders errors as JSON and results compactly for AI coding agents (duckdb -help lists all options)"
    )

def test_no_exit_hint_on_success(shell):
    test = ShellTest(shell).statement("SELECT 1")
    result = test.run()
    assert "hint:" not in result.stderr

def test_no_exit_hint_when_declined(shell):
    test = ShellTest(shell).add_argument("-no-agent").statement("SELEC 1")
    result = test.run()
    assert result.status_code == 1
    assert "hint:" not in result.stderr

def test_no_exit_hint_in_agent_mode(shell):
    test = agent_shell(shell).statement("SELEC 1")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr('"exception_type":"Parser"')
    assert "hint:" not in result.stderr

def test_streamed_error_without_agent(shell):
    test = ShellTest(shell).add_argument("-csv").statement("SELECT (1 / (r - 5))::INTEGER AS x FROM range(10) t(r)")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr("Conversion Error")

def test_agent_mode_environment_on(shell):
    # DUCKDB_AGENT_MODE=1 forces agent mode without any agent marker
    test = ShellTest(shell).env_var("DUCKDB_AGENT_MODE", "1").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("|---|")
    result.check_stderr("duckdb agent mode on (DUCKDB_AGENT_MODE is set)")

def test_agent_mode_environment_off(shell):
    # DUCKDB_AGENT_MODE=0 turns agent mode off even when an agent's marker is set
    test = agent_shell(shell).env_var("DUCKDB_AGENT_MODE", "0").statement(FIFTY_ROWS)
    result = test.run()
    assert "|---|" not in result.stdout
    assert "agent mode" not in result.stderr

def test_agent_mode_environment_flag_wins(shell):
    # -agent / -no-agent take precedence over DUCKDB_AGENT_MODE
    test = ShellTest(shell).env_var("DUCKDB_AGENT_MODE", "1").add_argument("-no-agent").statement(FIFTY_ROWS)
    result = test.run()
    assert "|---|" not in result.stdout
    test = agent_shell(shell).env_var("DUCKDB_AGENT_MODE", "0").add_argument("-agent").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("|---|")

@pytest.mark.parametrize("value", ["", "auto"])
def test_agent_mode_environment_auto(shell, value):
    # empty or auto leaves it to the auto-detection
    test = agent_shell(shell).env_var("DUCKDB_AGENT_MODE", value).statement(FIFTY_ROWS)
    result = test.run()
    result.check_stdout("|---|")
    test = ShellTest(shell).env_var("DUCKDB_AGENT_MODE", value).statement(FIFTY_ROWS)
    result = test.run()
    assert "|---|" not in result.stdout

def test_agent_mode_environment_invalid(shell):
    test = ShellTest(shell).env_var("DUCKDB_AGENT_MODE", "maybe").statement(FIFTY_ROWS)
    result = test.run()
    result.check_stderr("ignoring DUCKDB_AGENT_MODE=maybe")
    assert "|---|" not in result.stdout
