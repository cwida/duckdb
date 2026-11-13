# fmt: off

import pytest
from conftest import ShellTest

# the CLI opts in to the 'shell' grammar extension: bare expressions and table names run as statements

def test_bare_expression(shell):
    test = ShellTest(shell).statement("1 + 1;")
    result = test.run()
    result.check_stdout("2")

def test_subscript_expression(shell):
    test = ShellTest(shell).statement("[1,2,3][1];")
    result = test.run()
    result.check_stdout("1")

def test_method_call_expression(shell):
    test = ShellTest(shell).statement("('hello world').replace(' ', '_');")
    result = test.run()
    result.check_stdout("hello_world")

def test_bare_table_name(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE foo (i INTEGER);")
        .statement("INSERT INTO foo VALUES (1), (2), (3);")
        .statement(".mode json")
        .statement("foo;")
    )
    result = test.run()
    result.check_stdout(['[{"i":1},', '{"i":2},', '{"i":3}]'])

def test_qualified_table_name(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE SCHEMA s;")
        .statement("CREATE TABLE s.foo AS SELECT 42 AS i;")
        .statement("s.foo;")
        .statement("memory.s.foo;")
    )
    result = test.run()
    result.check_stdout("42")

def test_too_many_qualifiers(shell):
    test = ShellTest(shell).statement("a.b.c.d;")
    result = test.run()
    result.check_stderr("Too many qualifiers")

def test_aliases(shell):
    test = (
        ShellTest(shell)
        .statement(".mode json")
        .statement("foo: 1 + 1, bar: ('hello world').replace(' ', '_'), [1,2,3][1] AS list_element;")
    )
    result = test.run()
    result.check_stdout('[{"foo":2,"bar":"hello_world","list_element":1}]')

def test_implicit_alias_rejected(shell):
    test = ShellTest(shell).statement("1 + 1 foo;")
    result = test.run()
    result.check_stderr("syntax error")

def test_mixed_table_and_expression(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE foo (i INTEGER);")
        .statement("foo, 1 + 1;")
    )
    result = test.run()
    result.check_stderr("Mix of table names and expressions is not supported")

def test_typo_with_argument(shell):
    # "selec 42" is neither a statement nor an aliased expression
    test = ShellTest(shell).statement("selec 42;")
    result = test.run()
    result.check_stderr("syntax error")

def test_single_word_typo_is_table_lookup(shell):
    # a lone identifier is read as a table name - this is the side effect that is now CLI-only
    test = ShellTest(shell).statement("SEL;")
    result = test.run()
    result.check_stderr("Table with name SEL does not exist")

def test_shell_grammar_registered(shell):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement("SELECT name FROM duckdb_grammar_extensions();")
        .statement("SELECT current_setting('active_grammar_extensions');")
    )
    result = test.run()
    result.check_stdout("shell")
    result.check_stdout("[shell]")

def test_opt_out(shell):
    test = (
        ShellTest(shell)
        .statement("SET active_grammar_extensions=[];")
        .statement("1 + 1;")
    )
    result = test.run()
    result.check_stderr("syntax error")

def test_remove_grammar_extension(shell):
    test = (
        ShellTest(shell)
        .statement("CALL remove_grammar_extension('shell');")
        .statement("SELECT current_setting('active_grammar_extensions');")
    )
    result = test.run()
    result.check_stdout("[]")
    test = (
        ShellTest(shell)
        .statement("CALL remove_grammar_extension('shell');")
        .statement("1 + 1;")
    )
    result = test.run()
    result.check_stderr("syntax error")

def test_add_grammar_extension_is_idempotent(shell):
    test = (
        ShellTest(shell)
        .statement("CALL add_grammar_extension('shell');")
        .statement("SELECT current_setting('active_grammar_extensions');")
        .statement("1 + 1;")
    )
    result = test.run()
    result.check_stdout("[shell]")
    result.check_stdout("2")

def test_survives_reopen(shell):
    test = (
        ShellTest(shell)
        .statement(".open :memory:")
        .statement("1 + 1;")
    )
    result = test.run()
    result.check_stdout("2")

def test_builtin_type_literal(shell):
    test = ShellTest(shell).statement("DATE '2020-01-01';")
    result = test.run()
    result.check_stdout("2020-01-01")
