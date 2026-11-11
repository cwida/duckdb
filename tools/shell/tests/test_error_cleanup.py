import os
import subprocess
from pathlib import Path

import pytest

from conftest import ShellTest


def run_shell(shell, database, arguments, stdin="", init_file=None):
    return subprocess.run(
        [
            shell,
            *(["-init", str(init_file)] if init_file else ["-no-init"]),
            "-batch",
            *([str(database)] if database else []),
            *arguments,
        ],
        input=stdin,
        encoding="utf-8",
        capture_output=True,
        timeout=30,
        env=ShellTest(shell).get_environment(),
    )


def assert_checkpointed(database):
    # Check before reopening: recovery would otherwise hide the skipped destructor.
    assert database.is_file()
    assert not Path(str(database) + ".wal").exists()


@pytest.mark.parametrize("mode", ["-c", "-cmd", "-f", "stdin", "nested-read", "init", "positional"])
@pytest.mark.parametrize("bail", ["on", "off"])
@pytest.mark.parametrize("exit_code", [2, 17])
def test_explicit_nonzero_exit_cleans_up_and_stops_all_input(shell, tmp_path, mode, bail, exit_code):
    database = tmp_path / "explicit_exit.duckdb"
    setup = "SET wal_autocheckpoint='1TB'; CREATE TABLE saved AS SELECT 42 AS value;"
    after_exit = "INSERT INTO saved VALUES (99); SELECT 'SHOULD_NOT_RUN';"
    exit_command = f".exit {exit_code}"
    arguments = ["-cmd", f".bail {bail}", "-cmd", setup]
    stdin = after_exit + "\n"
    init_file = None
    if mode in ("-c", "-cmd"):
        arguments += [mode, exit_command, "-cmd", after_exit]
    elif mode == "positional":
        arguments += [exit_command, after_exit]
    elif mode == "stdin":
        stdin = exit_command + "\n" + stdin
    else:
        script = tmp_path / "exit.sql"
        script.write_text(exit_command + "\n" + after_exit + "\n")
        if mode == "nested-read":
            outer = tmp_path / "outer.sql"
            outer.write_text(f'.read "{script.as_posix()}"\n{after_exit}\n')
            arguments += ["-cmd", f'.read "{outer.as_posix()}"', "-cmd", after_exit]
        elif mode == "init":
            # Init files run before any post-init callbacks, so create the WAL there.
            script.write_text(f".bail {bail}\n{setup}\n{exit_command}\n{after_exit}\n")
            init_file = script
            arguments = ["-cmd", after_exit]
        else:
            arguments += ["-f", str(script), "-cmd", after_exit]
    result = run_shell(shell, database, arguments, stdin, init_file=init_file)
    assert result.returncode == exit_code, result.stderr
    assert "SHOULD_NOT_RUN" not in result.stdout
    assert "Exited due to error" not in result.stderr
    assert_checkpointed(database)

    reopened = run_shell(shell, database, ["-csv", "-noheader", "-c", "SELECT * FROM saved;"])
    assert reopened.returncode == 0, reopened.stderr
    assert reopened.stdout.strip() == "42"


@pytest.mark.parametrize("command", [".exit", ".exit 0", ".quit 17"])
@pytest.mark.parametrize("mode", ["stdin", "-c", "nested-read"])
def test_successful_exit_retains_input_scope(shell, tmp_path, command, mode):
    database = tmp_path / "exit_scope.duckdb"
    setup = "SET wal_autocheckpoint='1TB'; CREATE TABLE saved AS SELECT 42 AS value;"
    arguments = ["-cmd", setup]
    stdin = ""
    if mode == "stdin":
        stdin = command + "\nSELECT 'SHOULD_NOT_RUN';\n"
    elif mode == "-c":
        arguments += ["-c", command, "-c", "SELECT 'CONTINUED';"]
    else:
        script = tmp_path / "exit.sql"
        script.write_text(command + "\nSELECT 'SHOULD_NOT_RUN';\n")
        stdin = f'.read "{script.as_posix()}"\nSELECT \'CONTINUED\';\n'
    result = run_shell(shell, database, arguments, stdin)
    assert result.returncode == 0, result.stderr
    assert "SHOULD_NOT_RUN" not in result.stdout
    assert ("CONTINUED" in result.stdout) == (mode != "stdin")
    assert_checkpointed(database)


@pytest.mark.skipif(os.name == "nt", reason="POSIX exit status normalization")
def test_negative_exit_status_cleans_up_database(shell, tmp_path):
    database = tmp_path / "negative_exit.duckdb"
    setup = "SET wal_autocheckpoint='1TB'; CREATE TABLE saved AS SELECT 42 AS value;"
    result = run_shell(shell, database, ["-c", setup, "-c", ".exit -1", "-c", "SELECT 'SHOULD_NOT_RUN';"])
    assert result.returncode == 255, result.stderr
    assert "SHOULD_NOT_RUN" not in result.stdout
    assert_checkpointed(database)


@pytest.mark.parametrize("mode", ["-c", "-cmd", "-f", "-ui", "missing-file", "meta-command", "ascii-import"])
def test_command_error_cleans_up_database(shell, tmp_path, mode):
    database = tmp_path / "cleanup.duckdb"
    setup = "SET wal_autocheckpoint='1TB'; CREATE TABLE saved AS SELECT 42 AS value;"
    failure = "SELECT * FROM missing_cleanup_table;"
    expected_error = "missing_cleanup_table"
    arguments = ["-c", setup]
    if mode == "-f":
        script = tmp_path / "error.sql"
        script.write_text(failure + "\nSELECT 'SHOULD_NOT_RUN';\n")
        arguments += ["-bail", "-f", str(script)]
    elif mode == "-ui":
        arguments += ["-cmd", ".ui_command missing_cleanup_function()", "-ui"]
        expected_error = "missing_cleanup_function"
    elif mode == "missing-file":
        arguments += ["-f", str(tmp_path / "missing_cleanup_file.sql")]
        expected_error = "missing_cleanup_file.sql"
    elif mode == "ascii-import":
        arguments += ["-c", ".import -ascii unused.csv saved"]
        expected_error = "-ascii mode is no longer supported"
    elif mode == "meta-command":
        arguments += ["-c", ".missing_cleanup_command"]
        expected_error = "missing_cleanup_command"
    else:
        arguments += [mode, failure]
    arguments += ["-c", "SELECT 'SHOULD_NOT_RUN';"]
    result = run_shell(shell, database, arguments, "SELECT 'STDIN_SHOULD_NOT_RUN';\n")
    assert result.returncode == 1, result.stderr
    assert expected_error in result.stderr
    assert "SHOULD_NOT_RUN" not in result.stdout
    assert_checkpointed(database)

    reopened = run_shell(shell, database, ["-csv", "-noheader", "-c", "SELECT * FROM saved;"])
    assert reopened.returncode == 0, reopened.stderr
    assert reopened.stdout.strip() == "42"


@pytest.mark.parametrize("mode", ["-c", "-cmd", "-f"])
def test_successful_command_cleans_up_database(shell, tmp_path, mode):
    database = tmp_path / "success.duckdb"
    sql = "CREATE TABLE saved AS SELECT 42 AS value; SELECT 'SUCCESS';"
    if mode == "-f":
        script = tmp_path / "success.sql"
        script.write_text(sql)
        arguments = [mode, str(script)]
    else:
        arguments = [mode, sql]
    result = run_shell(shell, database, arguments)
    assert result.returncode == 0, result.stderr
    assert "SUCCESS" in result.stdout
    assert_checkpointed(database)


@pytest.mark.parametrize("mode", ["-c", "-cmd", "-f", "ascii-import"])
def test_no_bail_retains_command_behavior(shell, tmp_path, mode):
    database = tmp_path / "no_bail.duckdb"
    arguments = ["-cmd", ".bail off", "-cmd", "CREATE TABLE saved AS SELECT 42 AS value;"]
    failure = "SELECT * FROM missing_cleanup_table;"
    if mode == "ascii-import":
        arguments += ["-c", ".import -ascii unused.csv saved"]
    elif mode == "-f":
        script = tmp_path / "no_bail.sql"
        script.write_text(failure + "\nSELECT 'FILE_CONTINUED';\n")
        arguments += [mode, str(script)]
    else:
        arguments += [mode, failure]
    arguments += ["-cmd", "SELECT 'NEXT_COMMAND';"]
    result = run_shell(shell, database, arguments, "SELECT 'STDIN_CONTINUED';\n")
    # -f still reports errors after processing the file; -c/-cmd honor .bail off.
    assert result.returncode == (1 if mode == "-f" else 0), result.stderr
    expected_error = "-ascii mode is no longer supported" if mode == "ascii-import" else "missing_cleanup_table"
    assert expected_error in result.stderr
    if mode == "-f":
        assert "FILE_CONTINUED" in result.stdout
        assert "NEXT_COMMAND" not in result.stdout
    else:
        assert "NEXT_COMMAND" in result.stdout
    assert ("STDIN_CONTINUED" in result.stdout) == (mode == "-cmd")
    assert_checkpointed(database)


@pytest.mark.parametrize("option", ["-help", "-version"])
def test_informational_exit_stays_successful(shell, tmp_path, option):
    result = run_shell(shell, tmp_path / "unused.duckdb", [option])
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("mode", ["-serve", "-connect", "parameter", "-manual"])
def test_configured_command_error_cleans_up_database(shell, tmp_path, mode):
    database = tmp_path / "configured.duckdb"
    # -connect rejects a positional database, so open it through a prior callback.
    arguments = ["-cmd", f'.open "{database.as_posix()}"', "-cmd", "CREATE TABLE saved AS SELECT 42 AS value;"]
    expected_error = "missing_cleanup_table"
    if mode == "-manual":
        arguments += ["-manual", "too.many.name.components"]
        expected_error = "not a valid function name"
    elif mode == "parameter":
        arguments += ["-cmd", ".serve_command .print {missing_cleanup_parameter}", "-serve"]
        expected_error = "missing_cleanup_parameter"
    else:
        command = ".serve_command" if mode == "-serve" else ".connect_command"
        arguments += ["-cmd", command + " SELECT * FROM missing_cleanup_table;", mode]
    arguments += ["-c", "SELECT 'SHOULD_NOT_RUN';"]
    result = run_shell(shell, None, arguments)
    assert result.returncode == 1, result.stderr
    assert expected_error in result.stderr
    assert "SHOULD_NOT_RUN" not in result.stdout
    assert_checkpointed(database)


@pytest.mark.parametrize("mode", ["-format", "-format-file"])
def test_format_failure_stops_following_commands(shell, tmp_path, mode):
    database = tmp_path / "format.duckdb"
    # Fail binding the formatter query before function lookup, with or without autocomplete.
    setup = "CREATE TABLE saved AS SELECT 42 AS value; SET max_expression_depth=1;"
    arguments = ["-c", setup, mode]
    if mode == "-format-file":
        script = tmp_path / "format.sql"
        script.write_text("select 1")
        arguments.append(str(script))
    arguments += ["-c", "SELECT 'SHOULD_NOT_RUN';"]
    result = run_shell(shell, database, arguments, "select 1")
    assert result.returncode == 1, result.stderr
    assert "Max expression depth limit of 1 exceeded" in result.stderr
    assert "SHOULD_NOT_RUN" not in result.stdout
    assert_checkpointed(database)


@pytest.mark.parametrize("output_command", [".once -e", ".excel"])
@pytest.mark.parametrize("failure", [".exit 3", "SELECT * FROM missing_cleanup_table;"])
def test_early_exit_removes_temporary_output(shell, tmp_path, monkeypatch, output_command, failure):
    temporary_output = tmp_path / "output"
    temporary_output.mkdir()
    monkeypatch.setenv("TEMP", str(temporary_output))
    monkeypatch.setenv("TMP", str(temporary_output))
    database = tmp_path / "temporary_output.duckdb"
    setup = "SET wal_autocheckpoint='1TB'; CREATE TABLE saved AS SELECT 42 AS value;"
    result = run_shell(shell, database, ["-cmd", setup, "-cmd", output_command, "-c", failure])
    assert result.returncode == (3 if failure == ".exit 3" else 1), result.stderr
    assert not list(temporary_output.iterdir())
    assert_checkpointed(database)


@pytest.mark.parametrize("mode", ["-connect", "-serve"])
def test_invalid_connection_target_returns_requested_failure(shell, tmp_path, mode):
    result = run_shell(shell, None, [mode, "invalid_cleanup_target"])
    assert result.returncode == 1, result.stderr
    assert result.stderr
    assert "Exited due to error" not in result.stderr


@pytest.mark.skipif(os.name == "nt", reason="requires a POSIX pseudo-terminal")
@pytest.mark.parametrize("exit_command, exit_code", [(".exit 2", 2), (".exit 0", 0)])
def test_interactive_exit_saves_history(shell, tmp_path, monkeypatch, exit_command, exit_code):
    import pty

    history = tmp_path / "history"
    monkeypatch.setenv("DUCKDB_HISTORY", str(history))
    monkeypatch.setenv("TERM", "dumb")
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(
            [shell, "-no-init", "-interactive"],
            stdin=slave,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=ShellTest(shell).get_environment(),
        )
        os.close(slave)
        slave = None
        os.write(master, f"SELECT 26856;\n{exit_command}\n".encode())
        stdout, stderr = process.communicate(timeout=30)
        assert process.returncode == exit_code, stderr.decode()
        assert history.is_file(), stdout.decode()
        saved_history = history.read_text()
        assert "SELECT 26856;" in saved_history
        assert exit_command in saved_history
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate()
        os.close(master)
        if slave is not None:
            os.close(slave)


@pytest.mark.skipif(os.name == "nt", reason="requires a POSIX output pipe")
def test_explicit_exit_closes_output_pipe(shell, tmp_path):
    import shlex
    import sys

    completion = tmp_path / "pipe_finished"
    program = f"import sys; from pathlib import Path; sys.stdin.read(); Path({str(completion)!r}).write_text('closed')"
    script = tmp_path / "consumer.py"
    script.write_text(program)
    pipe = f'.output "|{shlex.quote(sys.executable)} {shlex.quote(str(script))}"'
    result = run_shell(shell, None, ["-cmd", pipe, "-c", ".exit 3"])
    assert result.returncode == 3, result.stderr
    # pclose must wait for the consumer to finish, not merely abandon the pipe on process exit.
    assert completion.read_text() == "closed"


@pytest.mark.parametrize("exit_code", [0, 3])
def test_database_shutdown_keeps_output_open(shell, tmp_path, exit_code):
    database = tmp_path / "shutdown_logging.duckdb"
    output = tmp_path / "shutdown.log"
    setup = (
        "CALL enable_logging(level='TRACE', storage='shell_log_storage');"
        "SET wal_autocheckpoint='1TB'; CREATE TABLE saved AS SELECT 42 AS value;"
    )
    result = run_shell(
        shell, database, ["-cmd", f'.output "{output.as_posix()}"', "-c", setup, "-c", f".exit {exit_code}"]
    )
    assert result.returncode == exit_code, result.stderr
    assert result.stdout == ""
    # The pending WAL is checkpointed during database destruction, before output is closed.
    assert "Start Checkpoint" in output.read_text(encoding="utf-8")
    assert_checkpointed(database)


def test_cleanup_subprocess_ignores_agent_environment(shell, tmp_path, monkeypatch):
    monkeypatch.setenv("AI_AGENT", "cleanup-regression")
    result = run_shell(shell, None, ["-c", "SELECT 42 AS value;"])
    assert result.returncode == 0, result.stderr
    assert "agent mode" not in result.stderr
