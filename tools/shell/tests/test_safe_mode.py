# fmt: off

import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest
from tools.shell.tests.conftest import random_filepath


@pytest.mark.parametrize("command", [".sh ls", ".cd ..", ".log file", ".import file.csv tbl", ".open new_file", ".output out", ".once out", ".excel out", ".read myfile.sql", ".pager cat"])
def test_safe_mode_command(shell, command):
    test = (
        ShellTest(shell, ['-safe'])
        .statement(command)
    )
    result = test.run()
    result.check_stderr('cannot be used in -safe mode')


@pytest.mark.parametrize("param", [(".sh ls", 'cannot be used in -safe mode'), ("INSTALL extension", "Permission Error")])
def test_safe_mode_dot_command(shell, param):
    command = param[0]
    expected_error = param[1]
    test = (
        ShellTest(shell)
        .statement('.safe_mode')
        .statement(command)
    )
    result = test.run()
    result.check_stderr(expected_error)

def test_safe_mode_database_basic(shell, random_filepath):
    test = (
        ShellTest(shell, [random_filepath, '-safe'])
        .statement('CREATE TABLE integers(i INT)')
        .statement('INSERT INTO integers VALUES (1), (2), (3)')
        .statement('SELECT SUM(i) FROM integers')
    )
    result = test.run()
    result.check_stdout("6")

@pytest.mark.parametrize("command", [".sh ls", ".cd ..", ".log file", ".import file.csv tbl", ".open new_file", ".output out", ".once out", ".excel out", ".read myfile.sql", ".pager cat"])
@pytest.mark.parametrize("persistent", [False, True])
def test_safe_mode_database_commands(shell, random_filepath, command, persistent):
    arguments = ['-safe'] if not persistent else [random_filepath, '-safe']
    test = (
        ShellTest(shell, arguments)
        .statement(command)
    )
    result = test.run()
    result.check_stderr('cannot be used in -safe mode')

@pytest.mark.parametrize("sql", ["COPY (SELECT 42) TO 'test.csv'", "LOAD spatial", "INSTALL spatial", "ATTACH 'file.db' AS file"])
@pytest.mark.parametrize("persistent", [False, True])
def test_safe_mode_query(shell, random_filepath, sql, persistent):
    arguments = ['-safe'] if not persistent else [random_filepath, '-safe']
    test = (
        ShellTest(shell, arguments)
        .statement(sql)
    )
    result = test.run()
    result.check_stderr('disabled')


def test_lock_config(shell, random_filepath):
    arguments = ['-safe']
    test = (
        ShellTest(shell, arguments)
        .statement("SET memory_limit='-1'")
    )
    result = test.run()
    result.check_stderr('locked')

# fmt: on
