import pytest

from conftest import ShellTest


def test_trace(shell, tmp_path):
    temp_dir = tmp_path / 'http_logging_dir'
    temp_dir.mkdir()
    temp_file = temp_dir / 'myfile'

    test = (
        ShellTest(shell)
        .statement("CALL enable_logging('FileSystem', level = 'trace', storage = 'shell_log_storage');")
        .statement(f"copy (select 1 as a) to '{temp_file.as_posix()}'")
    )
    result = test.run()
    result.check_stdout("TRACE:")


# FIXME: DUCKDB_LOG_DEBUG hasn't been used anywhere yet.
# def test_debug(shell):
#     test = (
#         ShellTest(shell)
#         .statement("CALL enable_logging(level = 'debug', storage = 'shell_log_storage');")
#         .statement("SELECT 42;")
#     )
#
#     result = test.run()
#     result.check_stdout("DEBUG:")


def test_info(shell):
    probe = ShellTest(shell).statement("LOAD HTTP;").run()
    if probe.status_code != 0:
        pytest.skip("DuckDB HTTP extension not installed/available")

    test = (
        ShellTest(shell)
        .statement("CALL enable_logging(level = 'info', storage = 'shell_log_storage');")
        .statement("LOAD HTTP;")
    )

    result = test.run()
    result.check_stdout("INFO:")
    result.check_stdout("LOAD HTTP")


@pytest.mark.skip(reason="Needs a new warning")
def test_warning(shell):
    test = ShellTest(shell).statement("SELECT list_transform([1], x -> x);")

    result = test.run()
    result.check_stdout("WARNING:")
    result.check_stdout("Deprecated lambda arrow (->) detected.")
    result.check_stdout("[1]")
    result.check_stderr(None)


@pytest.mark.skip(reason="Needs a new warning")
def test_warning_as_error(shell):
    test = ShellTest(shell)
    test.statement("SET warnings_as_errors = true")
    test.statement("SELECT list_transform([1], x -> x + 1)")
    result = test.run()
    result.check_stderr("Deprecated lambda arrow (->) detected.")
    assert result.status_code == 1


# Make sure that the same warning is not printed multiple times
@pytest.mark.skip(reason="Needs a new warning")
def test_multiple_warnings(shell):
    test = ShellTest(shell).statement(
        "select list_filter([1,2,3], x -> x > 2) l1, list_filter([1,2,3], x -> x > 2) l2, list_filter([1,2,3], x -> x > 2) l3;"
    )

    result = test.run()
    assert result.stdout.count("WARNING:") == 1
    result.check_stdout("Deprecated lambda arrow (->) detected.")
    result.check_stdout("│ [3]     │ [3]     │ [3]     │")


def test_changing_logging_settings(shell, tmp_path):
    log_dir = tmp_path / 'log_dir'
    test = ShellTest(shell).statement(
        f"CALL enable_logging(storage = 'file', storage_config = {{'path': '{log_dir.as_posix()}'}});"
    )

    result = test.run()
    print(result.stderr)
    result.check_stdout("WARNING:")
    result.check_stdout("The logging settings have been changed")
