# fmt: off

import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest, assert_loaded
import os
from pathlib import Path

def test_missing_arg(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT 42")
        .add_argument("-xyz")
    )
    result = test.run()
    result.check_stderr("Unrecognized option")
    result.check_stderr("xyz")

def test_headers(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT 42 as wilbur")
        .add_argument("-csv", "-header")
    )
    result = test.run()
    result.check_stdout("wilbur")

def test_no_headers(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT 42 as wilbur")
        .add_argument("-csv", "-noheader")
    )
    result = test.run()
    result.check_not_exist("wilbur")
def test_storage_version_latest(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT 42")
        .add_argument("-storage_version", "latest")
    )
    result = test.run()
    result.check_stdout("42")

def test_command(shell):
    test = (
        ShellTest(shell)
        .add_argument("-c", "SELECT 42")
    )
    result = test.run()
    result.check_stdout("42")

def test_command_parser_setting_applies_to_later_statements(shell):
    # a SET that changes parser behavior must reach the following statements of the same -c string
    test = (
        ShellTest(shell)
        .add_argument("-csv", "-c", "SET integer_division=true; SELECT 2/4 AS q")
    )
    result = test.run()
    result.check_stdout("0")
    result.check_not_exist("0.5")

def test_command_identifier_case_applies_to_later_statements(shell):
    test = (
        ShellTest(shell)
        .add_argument("-csv", "-c", "SET preserve_identifier_case=false; SELECT 42 AS MyColumn")
    )
    result = test.run()
    result.check_stdout("mycolumn")
    result.check_not_exist("MyColumn")

def test_command_unterminated_block_comment(shell):
    test = ShellTest(shell).add_argument("-c", "SELECT 1/*;")
    result = test.run()
    assert result.status_code == 1
    assert result.stdout == ""
    result.check_stderr('Parser Error: unterminated /* comment at or near "/*;"')

def test_version(shell):
    test = (
        ShellTest(shell)
        .add_argument("-version")
    )
    result = test.run()
    result.check_stdout("v")

def test_csv_options(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT 42 a, NULL b")
        .add_argument("-csv", "-nullvalue", "MYNULL", "-separator", "MYSEP")
    )
    result = test.run()
    result.check_stdout("42MYSEPMYNULL")

def test_echo(shell):
    test = (
        ShellTest(shell, ['-echo'])
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stdout("SELECT 42")

def test_storage_version_error(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT 42")
        .add_argument("-storage-version", "XXX")
    )
    result = test.run()
    result.check_stderr("XXX")

# `-serve`/`-connect` only expand their command template - quack resolves the secret, its token and the
# endpoint on its own. The probes print the expanded command instead of running it.
SERVE_PROBE = '.serve_command ".print serve -> quack_serve(create_secret_if_not_exists=true{serve_secret|})"'
CONNECT_PROBE = '.connect_command ".print connect -> CONNECT {type|quack}:{connect_secret|}"'

def test_serve_defaults(shell):
    result = ShellTest(shell).add_argument("-cmd", SERVE_PROBE, "-serve", "-no-stdin").run()
    result.check_stdout("serve -> quack_serve(create_secret_if_not_exists=true)")

def test_connect_defaults(shell):
    result = ShellTest(shell).add_argument("-cmd", CONNECT_PROBE, "-connect", "-no-stdin").run()
    result.check_stdout("connect -> CONNECT quack:")

def test_serve_missing_parameter(shell):
    test = (
        ShellTest(shell)
        .add_argument("-cmd", ".serve_command .print {unknown_parameter}", "-serve", "-no-stdin")
    )
    result = test.run()
    result.check_stderr("no value provided for parameter 'unknown_parameter'")

def test_connect_type_defaults_to_quack(shell):
    result = ShellTest(shell).add_argument("-cmd", CONNECT_PROBE, "-connect", "-no-stdin").run()
    result.check_stdout("connect -> CONNECT quack:")

def test_connect_type_argument(shell):
    result = ShellTest(shell).add_argument("-cmd", CONNECT_PROBE, "-connect", "postgres", "-no-stdin").run()
    result.check_stdout("connect -> CONNECT postgres:")

def test_connect_type_does_not_swallow_an_option(shell):
    # the optional argument is only taken when it is not an option itself
    result = ShellTest(shell).add_argument("-cmd", CONNECT_PROBE, "-connect", "-no-stdin").run()
    result.check_stdout("connect -> CONNECT quack:")

def test_connect_type_with_secret(shell):
    test = ShellTest(shell).add_argument("-cmd", CONNECT_PROBE, "-connect", "postgres:s1", "-no-stdin")
    result = test.run()
    result.check_stdout("connect -> CONNECT postgres:")
    result.check_stdout("SECRET s1")

def test_serve_secret(shell):
    test = ShellTest(shell).add_argument("-cmd", SERVE_PROBE, "-serve", "quack:s1", "-no-stdin")
    result = test.run()
    result.check_stdout("serve -> quack_serve(create_secret_if_not_exists=true, secret='s1')")

def test_serve_type_quack_is_accepted(shell):
    result = ShellTest(shell).add_argument("-cmd", SERVE_PROBE, "-serve", "quack", "-no-stdin").run()
    result.check_stdout("serve -> quack_serve(create_secret_if_not_exists=true)")

@pytest.mark.parametrize("argument", ["postgres", "postgres:s1", "s1"])
def test_serve_rejects_other_types(shell, argument):
    # only quack can be served - a bare name is a type, so `-serve s1` is rejected too
    result = ShellTest(shell).add_argument("-serve", argument, "-no-stdin").run()
    result.check_stderr("only 'quack' is supported by '-serve'")

@pytest.mark.parametrize("flag", ["-serve", "-connect"])
@pytest.mark.parametrize("argument", ["", "quack:", ":s1"])
def test_connection_target_must_be_well_formed(shell, flag, argument):
    result = ShellTest(shell).add_argument(flag, argument, "-no-stdin").run()
    result.check_stderr("expected TYPE[:SECRET]")

def test_connect_rejects_a_database_argument(shell, tmp_path):
    db = tmp_path / "some.db"
    test = ShellTest(shell).add_argument(str(db), "-connect", "-no-stdin")
    result = test.run()
    result.check_stderr("cannot open a database")
    result.check_stderr("-connect")
    # the database is rejected before it is opened, so it is never created
    assert not db.exists()

def test_serve_accepts_a_database_argument(shell, tmp_path):
    # -serve serves the database it is given, so a file is expected there
    db = tmp_path / "served.db"
    test = ShellTest(shell).add_argument(
        str(db), "-cmd", SERVE_PROBE, "-serve", "-no-stdin"
    )
    result = test.run()
    result.check_stdout("serve -> quack_serve(create_secret_if_not_exists=true)")

def test_serve_command_runs_against_quack(shell):
    # the built-in commands have to be valid quack SQL - a bad one would fail at parse/bind time
    assert_loaded(shell, "quack")
    result = ShellTest(shell).add_argument("-cmd", "LOAD quack;", "-connect", "quack:nope", "-no-stdin").run()
    result.check_stderr('Secret with name "nope" not found')


# fmt: on
