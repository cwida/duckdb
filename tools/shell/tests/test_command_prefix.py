# a dot-command resolves by its full name, or by an abbreviation it declares; any other prefix is not a command

import pytest
from conftest import ShellTest


@pytest.mark.parametrize(
    "abbreviation,full",
    [
        (".sep |", ".separator"),
        (".col", ".columns"),
        (".header off", ".headers"),
        (".ta", ".tables"),
        (".bai off", ".bail"),
        (".sh true", ".shell"),
        (".null -", ".nullvalue"),
    ],
)
def test_declared_abbreviations_resolve(shell, abbreviation, full):
    test = ShellTest(shell).statement(abbreviation)
    result = test.run()
    result.check_not_exist("Unrecognized command")


def test_separator_abbreviation(shell):
    test = ShellTest(shell).statement(".sep |").statement(".mode list").statement("SELECT 1 AS a, 2 AS b")
    result = test.run()
    result.check_stdout("1|2")


@pytest.mark.parametrize(
    "command,suggestion",
    [
        (".sche", ".schema"),
        (".serve 'xx'", ".serve_command"),
        (".connect off", ".connect_command"),
        (".ui 1", ".ui_command"),
        (".safe", ".safe_mode"),
        (".progress on", ".progress_bar"),
        (".high", ".highlight"),
        (".mod csv", ".mode"),
    ],
)
def test_other_prefixes_are_not_commands(shell, command, suggestion):
    result = ShellTest(shell).statement(command).run()
    result.check_stderr(f"Unrecognized command '{command.split()[0][1:]}'")
    result.check_stderr(suggestion)


def test_short_abbreviation_is_refused(shell):
    # .ba is shorter than the three letters .bail declares
    result = ShellTest(shell).statement(".ba on").run()
    result.check_stderr("Unrecognized command 'ba'")
