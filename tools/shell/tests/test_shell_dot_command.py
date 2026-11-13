# fmt: off

from conftest import ShellTest


def test_dot_version(shell):
    test = ShellTest(shell).statement(".version")
    result = test.run()
    result.check_stdout("DuckDB")
    result.check_stdout("v")
    assert any(token in result.stdout for token in ("clang-", "gcc-", "msvc-"))


def test_dot_version_prefix(shell):
    test = ShellTest(shell).statement(".ver")
    result = test.run()
    result.check_stdout("DuckDB")
    result.check_stdout("v")


def test_call_shell_dot_command_version(shell):
    test = ShellTest(shell).statement("CALL shell_dot_command_version('', '')")
    result = test.run()
    result.check_stdout("print")
    result.check_stdout("DuckDB")


def test_help_lists_catalog_version(shell):
    test = ShellTest(shell).statement(".help")
    result = test.run()
    result.check_stdout(".version")
    result.check_stdout("Show the version")


def test_catalog_plugin_macro(shell):
    test = (
        ShellTest(shell)
        .statement(
            "CREATE MACRO shell_dot_command_hello(user_input, extra_info) AS TABLE "
            "SELECT 'print' AS command, 'hello from plugin' AS input"
        )
        .statement(".hello")
    )
    result = test.run()
    result.check_stdout("hello from plugin")


def test_unknown_dot_command(shell):
    test = ShellTest(shell).statement(".not_a_real_dot_command")
    result = test.run()
    result.check_stderr("Unrecognized command")


def test_help_lists_plugin_macro(shell):
    test = (
        ShellTest(shell)
        .statement(
            "CREATE MACRO shell_dot_command_hello(user_input, extra_info) AS TABLE "
            "SELECT 'print' AS command, 'hello from plugin' AS input"
        )
        .statement(".help hello")
    )
    result = test.run()
    result.check_stdout(".hello")


def test_dot_commands_do_not_abort_after_failed_transaction(shell):
    test = (
        ShellTest(shell)
        .statement(".bail off")
        .statement("BEGIN")
        .statement("SELECT * FROM this_table_does_not_exist")
        .statement(".version")
        .statement(".not_a_real_dot_command")
        .statement(".help")
    )
    result = test.run()
    assert "Exited due to error" not in result.stderr
    result.check_stderr("Unrecognized command")
    assert "Show help text for PATTERN" in result.stdout
