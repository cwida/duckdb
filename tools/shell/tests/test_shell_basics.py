# fmt: off

import os
import re
import subprocess

import pytest
from conftest import ShellTest


def test_basic(shell):
    test = ShellTest(shell).statement("select 'asdf' as a")
    result = test.run()
    result.check_stdout("asdf")


def test_range(shell):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement("select * from range(10000)")
    )
    result = test.run()
    result.check_stdout("9999")


@pytest.mark.parametrize('generated_file', ["col_1,col_2\n1,2\n10,20"], indirect=True)
def test_import(shell, generated_file):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement(f'.import "{generated_file.as_posix()}" test_table')
        .statement("select * FROM test_table")
    )

    result = test.run()
    result.check_stdout("col_1,col_2\r\n1,2\r\n10,20")


@pytest.mark.parametrize('generated_file', ["42\n84"], indirect=True)
def test_import_sum(shell, generated_file):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE a (i INTEGER)")
        .statement(f'.import "{generated_file.as_posix()}" a')
        .statement("SELECT SUM(i) FROM a")
    )

    result = test.run()
    result.check_stdout("126")


def test_pragma(shell):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement(".headers off")
        .statement(".sep |")
        .statement('.nullvalue ""')
        .statement("CREATE TABLE t0(c0 INT);")
        .statement("PRAGMA table_info('t0');")
    )

    result = test.run()
    result.check_stdout("0|c0|INTEGER|false||false")


def test_system_functions(shell):
    test = ShellTest(shell).statement("SELECT 1, current_query() as my_column")

    result = test.run()
    result.check_stdout("SELECT 1, current_query() as my_column")


@pytest.mark.parametrize(
    ["input", "output"],
    [
        ("select LIST_VALUE(1, 2)", "[1, 2]"),
        ("select STRUCT_PACK(x := 3, y := 3)", "{'x': 3, 'y': 3}"),
        ("select STRUCT_PACK(x := 3, y := LIST_VALUE(1, 2))", "{'x': 3, 'y': [1, 2]}"),
    ],
)
def test_nested_types(shell, input, output):
    test = ShellTest(shell).statement(input)
    result = test.run()
    result.check_stdout(output)


def test_invalid_cast(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE a (i STRING)")
        .statement("INSERT INTO a VALUES ('XXXX')")
        .statement("SELECT CAST(i AS INTEGER) FROM a")
    )
    result = test.run()
    result.check_stderr("Could not convert")

def test_newline_in_value(shell):
    test = (
        ShellTest(shell)
        .statement("""select 'hello
world' as a""")
    )

    result = test.run()
    result.check_stdout("hello\\nworld")

def test_newline_in_column_name(shell):
    test = (
        ShellTest(shell)
        .statement("""select 42 as "hello
world" """)
    )

    result = test.run()
    result.check_stdout("hello\\nworld")

def test_bail_on_stops_after_error(shell):
    test = (
        ShellTest(shell)
        .statement(".bail on")
        .statement("invalid sql;")
        .statement("select 'should not reach here'")
    )

    result = test.run()
    assert result.status_code == 1
    assert result.stdout == ""


def test_bail_off_continues_after_error(shell):
    test = (
        ShellTest(shell)
        .statement(".bail off")
        .statement("invalid sql;")
        .statement("select 'reached here'")
    )

    result = test.run()
    result.check_stderr("Parser Error: syntax error at or near")
    assert "reached here" in str(result.stdout)

def test_home_duckdbrc_is_not_read(shell, tmp_path):
    (tmp_path / '.duckdbrc').write_text('.print init-output\n')
    for arguments in ([], ['-csv']):
        test = ShellTest(shell, arguments).env_var('HOME', tmp_path.as_posix()).env_var('USERPROFILE', str(tmp_path))
        test = test.statement("SELECT 42;")
        result = test.run()
        result.check_stdout('42')
        result.check_not_exist('init-output')


def test_shell_test_adds_no_init_once(shell):
    arguments = ['-csv']
    assert ShellTest(shell, arguments).arguments.count('--no-init') == 1
    assert arguments == ['-csv']
    for _ in range(2):
        assert ShellTest(shell).arguments.count('--no-init') == 1
    assert '--no-init' not in ShellTest(shell, ['-init', '/dev/null']).arguments


def test_bail_on_missing_init(shell):
    test = (
        ShellTest(shell, ['-init', '___thisfiledoesnotexist'])
        .statement("select 'reached here'")
    )

    result = test.run()
    result.check_stderr("___thisfiledoesnotexist")
    assert "reached here" not in str(result.stdout)

@pytest.mark.parametrize('generated_file', ["selec 42;"], indirect=True)
def test_bail_within_init(shell, generated_file):
    test = (
        ShellTest(shell, ['-init', generated_file.as_posix()])
        .statement("select 'reached here'")
    )

    result = test.run()
    result.check_stderr("selec")
    assert "reached here" not in str(result.stdout)

@pytest.mark.parametrize('generated_file', ["selec 42;\nselect 'reached here'"], indirect=True)
def test_bail_within_read(shell, generated_file):
    test = (
        ShellTest(shell)
        .statement(".read \"" + generated_file.as_posix() + "\"")
    )

    result = test.run()
    result.check_stderr("selec")
    assert "reached here" not in str(result.stdout)
@pytest.mark.parametrize('generated_file', [".bail off\nselec 42;"], indirect=True)
def test_explicit_bail_within_init(shell, generated_file):
    test = (
        ShellTest(shell, ['-init', generated_file.as_posix()])
        .statement("select 'reached here'")
    )

    result = test.run()
    result.check_stderr("selec")
    assert "reached here" in str(result.stdout)

@pytest.mark.parametrize('generated_file', ["selec 42;\nselect 'reached here'"], indirect=True)
def test_explicit_bail_within_read(shell, generated_file):
    test = (
        ShellTest(shell)
        .statement(".bail off")
        .statement(".read \"" + generated_file.as_posix() + "\"")
    )

    result = test.run()
    result.check_stderr("selec")
    assert "reached here" in str(result.stdout)

@pytest.mark.skipif(os.name == 'nt', reason="Skipped on windows")
def test_shell_command(shell):
    test = (
        ShellTest(shell)
        .statement(".shell echo quack")
    )
    result = test.run()
    result.check_stdout("quack")

def test_cd(shell, tmp_path):
    pwd_dir = os.getcwd()

    pwd_test = (
        ShellTest(shell)
        .statement(".shell pwd")
    )
    pwd_result = pwd_test.run()
    pwd_result.check_stdout('duckdb')

    random_dir_test = (
        ShellTest(shell)
        .statement(f".cd {tmp_path.as_posix()}")
    )
    random_dir_result = random_dir_test.run()
    random_dir_result.check_not_exist(pwd_dir)
    random_dir_result.check_stderr(None)

def test_changes_on(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE a (I INTEGER)")
        .statement(".changes on")
        .statement("INSERT INTO a VALUES (42)")
        .statement("INSERT INTO a VALUES (42)")
        .statement("INSERT INTO a VALUES (42)")
        .statement("DROP TABLE a")
    )
    result = test.run()
    result.check_stdout("total_changes: 3")

def test_changes_off(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE a (I INTEGER);")
        .statement(".changes off")
        .statement("INSERT INTO a VALUES (42);")
        .statement("DROP TABLE a;")
    )
    result = test.run()
    result.check_not_exist("changes:")
    result.check_stdout(None)

def test_echo(shell):
    test = (
        ShellTest(shell)
        .statement(".echo on")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stdout("SELECT 42")

def test_invalid_sql(shell):
    test = ShellTest(shell).statement("invalid command;")
    result = test.run()
    assert result.status_code == 1
    result.check_stderr("Parser Error: syntax error at or near")

@pytest.mark.parametrize("alias", ["exit", "quit"])
def test_exit(shell, alias):
    test = ShellTest(shell).statement(f".{alias}").statement("invalid command;")
    result = test.run()
    # Shows that the exit & quit dot commands exit the shell prior to the error
    # Still indirect but ensures a failure if they do not work as expected
    assert result.status_code == 0

def test_exit_rc(shell):
    test = ShellTest(shell).statement(".exit 17")
    result = test.run()
    assert result.status_code == 17

def test_print(shell):
    test = ShellTest(shell).statement(".print asdf")
    result = test.run()
    result.check_stdout("asdf")

def test_headers(shell):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement(".headers on")
        .statement("SELECT 42 as wilbur")
    )
    result = test.run()
    result.check_stdout("wilbur")

def test_like(shell):
    test = ShellTest(shell).statement("select 'yo' where 'abc' like 'a%c'")
    result = test.run()
    result.check_stdout("yo")

def test_regexp_matches(shell):
    test = ShellTest(shell).statement("select regexp_matches('abc','abc')")
    result = test.run()
    result.check_stdout("true")

def test_help(shell):
    test = (
        ShellTest(shell)
        .statement(".help")
    )
    result = test.run()
    result.check_stdout("Show help text for PATTERN")

def test_load_error(shell, random_filepath):
    test = (
        ShellTest(shell)
        .statement(f".load {random_filepath.as_posix()}")
    )
    result = test.run()
    result.check_stderr("Error")

def test_streaming_error(shell):
    test = (
        ShellTest(shell)
        .statement("SELECT x::INT FROM (SELECT x::VARCHAR x FROM range(10) tbl(x) UNION ALL SELECT 'hello' x) tbl(x);")
    )
    result = test.run()
    result.check_stderr("Could not convert string")

@pytest.mark.parametrize("stmt", [
    "explain select sum(i) from range(1000) tbl(i)",
    "explain analyze select sum(i) from range(1000) tbl(i)"
])
def test_explain(shell, stmt):
    test = (
        ShellTest(shell)
        .statement(stmt)
    )
    result = test.run()
    result.check_stdout("RANGE")

def test_returning_insert(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE table1 (a INTEGER DEFAULT -1, b INTEGER DEFAULT -2, c INTEGER DEFAULT -3);")
        .statement("INSERT INTO table1 VALUES (1, 2, 3) RETURNING *;")
        .statement("SELECT COUNT(*) FROM table1;")
    )
    result = test.run()
    result.check_stdout("1")

def test_pragma_display(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE table1 (mylittlecolumn INTEGER);")
        .statement("pragma table_info('table1');")
    )
    result = test.run()
    result.check_stdout("mylittlecolumn")

def test_show_display(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE table1 (mylittlecolumn INTEGER);")
        .statement("show table1;")
    )
    result = test.run()
    result.check_stdout("mylittlecolumn")

def test_call_display(shell):
    test = (
        ShellTest(shell)
        .statement("CALL range(4);")
    )
    result = test.run()
    result.check_stdout("3")

def test_execute_display(shell):
    test = (
        ShellTest(shell)
        .statement("PREPARE v1 AS SELECT ?::INT;")
        .statement("EXECUTE v1(42);")
    )
    result = test.run()
    result.check_stdout("42")

@pytest.mark.parametrize('generated_file', ["select 42"], indirect=True)
def test_read(shell, generated_file):
    test = (
        ShellTest(shell)
        .statement(f".read {generated_file.as_posix()}")
    )
    result = test.run()
    result.check_stdout("42")

def test_recursive_read(shell, tmp_path):
    sql_file = tmp_path / "recursive_read.sql"
    sql_file.write_text(f".read {sql_file.as_posix()}")
    result = ShellTest(shell).statement(f".read {sql_file.as_posix()}").run()
    result.check_stderr("recursive .read")

@pytest.mark.parametrize('generated_file', ["select 42"], indirect=True)
def test_execute_file(shell, generated_file):
    test = (
        ShellTest(shell, ['-f', generated_file.as_posix()])
    )
    result = test.run()
    result.check_stdout("42")

def test_execute_non_existent_file(shell):
    test = (
        ShellTest(shell, ['-f', '____this_file_does_not_exist'])
    )
    result = test.run()
    result.check_stderr("____this_file_does_not_exist")

@pytest.mark.parametrize('generated_file', ["insert into tbl values (42)"], indirect=True)
def test_execute_files(shell, generated_file):
    test = (
        ShellTest(shell, ['-c', 'CREATE TABLE tbl(i INT)', '-f', generated_file.as_posix(), '-f', generated_file.as_posix(), '-c', 'SELECT SUM(i) FROM tbl'])
    )
    result = test.run()
    result.check_stdout("84")

def test_show_basic(shell):
    test = (
        ShellTest(shell)
        .statement(".show")
    )
    result = test.run()
    result.check_stdout("rowseparator")

@pytest.mark.parametrize("cmd", [
    "vfsinfo",
    "vfsname",
    "vfslist",
])
def test_volatile_commands(shell, cmd):
    test = (
        ShellTest(shell)
        .statement(f".{cmd}")
    )
    result = test.run()
    assert result.status_code == 1
    result.check_stderr("Unknown Command Error")

@pytest.mark.parametrize("pattern", [
    "test",
    "tes%",
    "tes*",
    ""
])
def test_schema(shell, pattern):
    # .schema pretty-prints the statements using the SQL formatter by default
    test = (
        ShellTest(shell)
        .statement("create table test (a int, b varchar);")
        .statement("insert into test values (1, 'hello');")
        .statement(f".schema {pattern}")
    )
    result = test.run()
    result.check_stdout("CREATE TABLE test(\n    a INTEGER,\n    b VARCHAR\n);")

def test_schema_indent(shell):
    test = (
        ShellTest(shell)
        .statement("create table test (a int, b varchar, c int, d int, k int, primary key(a, b));")
        .statement(".schema -indent")
    )
    result = test.run()
    result.check_stdout("CREATE TABLE test(\n")

@pytest.mark.parametrize("option", ["--no-indent", "--no-format"])
def test_schema_no_indent(shell, option):
    # --no-indent / --no-format prints the statements as they are stored (single line)
    test = (
        ShellTest(shell)
        .statement("create table test (a int, b varchar);")
        .statement(f".schema {option}")
    )
    result = test.run()
    result.check_stdout("CREATE TABLE test(a INTEGER, b VARCHAR);")

def test_schema_unknown_option(shell):
    test = (
        ShellTest(shell)
        .statement("create table test (a int, b varchar);")
        .statement(".schema -x")
    )
    result = test.run()
    assert result.status_code == 1
    result.check_stderr('unknown option "-x"')

def test_tables(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE asda (i INTEGER);")
        .statement("CREATE TABLE bsdf (i INTEGER);")
        .statement("CREATE TABLE csda (i INTEGER);")
        .statement(".tables")
    )
    result = test.run()
    result.check_stdout("asda")
    result.check_stdout("bsdf")
    result.check_stdout("csda")

def test_tables_pattern(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE asda (i INTEGER);")
        .statement("CREATE TABLE bsdf (i INTEGER);")
        .statement("CREATE TABLE csda (i INTEGER);")
        .statement(".tables %da")
    )
    result = test.run()
    result.check_stdout("asda")
    result.check_stdout("csda")

def test_tables_schema_disambiguation(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE SCHEMA a;")
        .statement("CREATE SCHEMA b;")
        .statement("CREATE TABLE a.foobar(name VARCHAR);")
        .statement("CREATE TABLE b.foobar(name VARCHAR);")
        .statement(".tables")
    )
    result = test.run()
    result.check_stdout("foobar")

def test_tables_schema_filtering(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE SCHEMA a;")
        .statement("CREATE SCHEMA b;")
        .statement("CREATE TABLE a.foobar(name VARCHAR);")
        .statement("CREATE TABLE b.foobar(name VARCHAR);")
        .statement("CREATE TABLE a.unique_table(x INTEGER);")
        .statement("CREATE TABLE b.other_table(y INTEGER);")
        .statement(".tables a.%")
    )
    result = test.run()
    result.check_stdout("foobar")
    result.check_stdout("unique_table")

def test_tables_backward_compatibility(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE main_table(i INTEGER);")
        .statement("CREATE TABLE unique_table(x INTEGER);")
        .statement(".tables")
    )
    result = test.run()
    result.check_stdout("main_table")
    result.check_stdout("unique_table")

def test_tables_with_views(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE SCHEMA a;")
        .statement("CREATE SCHEMA b;")
        .statement("CREATE TABLE a.foobar(name VARCHAR);")
        .statement("CREATE TABLE b.foobar(name VARCHAR);")
        .statement("CREATE VIEW a.test_view AS SELECT 1 AS x;")
        .statement("CREATE VIEW b.test_view AS SELECT 2 AS y;")
        .statement(".tables")
    )
    result = test.run()
    result.check_stdout("foobar")
    result.check_stdout("test_view")
    result.check_stdout("foobar")
    result.check_stdout("test_view")

def test_indexes(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE a (i INTEGER);")
        .statement("CREATE INDEX a_idx ON a(i);")
        .statement(".indexes a%")
    )
    result = test.run()
    result.check_stdout("a_idx")

def test_schema_pattern_no_result(shell):
    test = (
        ShellTest(shell)
        .statement("create table testp (a integer)")
        .statement("create table test_p (b integer)")
        .statement(".schema %z%")
    )
    result = test.run()
    result.check_not_exist("CREATE TABLE")

def test_schema_pattern(shell):
    test = (
        ShellTest(shell)
        .statement("create table duckdb_p (a int, b varchar, c BIT);")
        .statement("create table p_duck(d INT, f DATE);")
        .statement(".schema %p")
    )
    result = test.run()
    result.check_stdout("CREATE TABLE duckdb_p(\n    a INTEGER,\n    b VARCHAR,\n    c BIT\n);")

@pytest.mark.skipif(os.name == 'nt', reason="Windows treats newlines in a problematic manner")
def test_schema_pattern_extended(shell):
    test = (
        ShellTest(shell)
        .statement("create table duckdb_p (a int, b varchar, c BIT);")
        .statement("create table p_duck(d INT, f DATE);")
        .statement(".schema %p%")
    )
    result = test.run()
    expected = [
        "CREATE TABLE duckdb_p(\n    a INTEGER,\n    b VARCHAR,\n    c BIT\n);",
        "CREATE TABLE p_duck(\n    d INTEGER,\n    f DATE\n);"
    ]
    result.check_stdout(expected)

def test_clone_error(shell):
    test = (
        ShellTest(shell)
        .statement(".clone")
    )
    result = test.run()
    result.check_stderr('Unrecognized command')

def test_sha3sum(shell):
    test = (
        ShellTest(shell)
        .statement(".sha3sum")
    )
    result = test.run()
    result.check_stderr('Unknown Command Error')

def test_jsonlines(shell):
    test = (
        ShellTest(shell)
        .statement(".mode jsonlines")
        .statement("SELECT 42,43;")
    )
    result = test.run()
    result.check_stdout('{"42":42,"43":43}')

def test_jsonlines_cmdline(shell):
    test = (
        ShellTest(shell, ['-jsonlines'])
        .statement("SELECT 42,43;")
    )
    result = test.run()
    result.check_stdout('{"42":42,"43":43}')

def test_nested_jsonlines(shell):
    test = (
        ShellTest(shell)
        .statement(".mode jsonlines")
        .statement("SELECT [1,2,3]::JSON AS x;")
    )
    result = test.run()
    result.check_stdout('{"x":[1,2,3]}')

def test_separator(shell):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement(".separator XX")
        .statement("SELECT 42,43;")
    )
    result = test.run()
    result.check_stdout('42XX43')

def test_timer(shell):
    test = (
        ShellTest(shell)
        .statement(".timer on")
        .statement("SELECT NULL;")
    )
    result = test.run()
    result.check_stdout('Run Time (s):')

def test_timer_digits(shell):
    test = (
        ShellTest(shell)
        .statement(".timer on 6")
        .statement("SELECT NULL;")
    )
    result = test.run()
    result.check_stdout('Run Time (s):')
    assert re.search(r'real \d\.\d{6} ', result.stdout)

@pytest.mark.parametrize("digits", ["10", "-1"])
def test_timer_digits_out_of_range(shell, digits):
    test = (
        ShellTest(shell)
        .statement(f".timer on {digits}")
        .statement("SELECT NULL;")
    )
    result = test.run()
    result.check_stderr('.timer DIGITS must be between 0 and 9')
    assert 'Run Time (s):' not in result.stdout

def test_output_csv_mode(shell, random_filepath):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement(f".output {random_filepath.as_posix()}")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.stdout = open(random_filepath, 'rb').read()
    result.check_stdout(b'42')

def test_issue_6204(shell, random_filepath):
    test = (
        ShellTest(shell)
        .statement(f".output {random_filepath.as_posix()}")
        .statement("select * from range(2049);")
    )
    result = test.run()
    result.check_stdout(None)

    with open(random_filepath, 'r', encoding='utf-8') as f:
        output = f.read()
        nums = set(int(x) for x in re.findall(r'\d+', output))

        assert all(i in nums for i in range(2049))

def test_once(shell, random_filepath):
    test = (
        ShellTest(shell)
        .statement(f".once {random_filepath.as_posix()}")
        .statement("SELECT 43;")
    )
    result = test.run()
    result.stdout = open(random_filepath, 'rb').read()
    result.check_stdout(b'43')

def test_output_off_no_error(shell):
    # .output off should suppress output without printing an error to stderr
    test = (
        ShellTest(shell)
        .statement(".output off")
    )
    result = test.run()
    assert "Error" not in result.stderr

def test_output_invalid_path_error(shell, tmp_path):
    # .output to an invalid path should print an error to stderr
    invalid_path = (tmp_path / "nonexistent_dir" / "file.txt").as_posix()
    test = (
        ShellTest(shell)
        .statement(f".output {invalid_path}")
    )
    result = test.run()
    result.check_stderr("Error: cannot write to")

def test_once_temp_file_cleanup(shell, tmp_path):
    # Verify that temp files created by .once are cleaned up
    # when a new temp file is created via NewTempFile -> ClearTempFile
    filepath1 = tmp_path / "first.txt"
    filepath2 = tmp_path / "second.txt"
    test = (
        ShellTest(shell)
        .statement(f".once {filepath1.as_posix()}")
        .statement("SELECT 'first'")
        .statement(f".once {filepath2.as_posix()}")
        .statement("SELECT 'second'")
        .statement(".output stdout")
        .statement("SELECT 'done'")
    )
    result = test.run()
    result.check_stdout("done")
    assert filepath2.exists()
    result.stdout = open(filepath2, 'rb').read()
    result.check_stdout(b'second')

@pytest.mark.parametrize("dot_command", [
    ".mode ascii",
    ""
])
def test_mode_ascii(shell, dot_command):
    args = ['-ascii'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT NULL, 42, 'fourty-two', 42.0;")
    )
    result = test.run()
    result.check_stdout('fourty-two')

@pytest.mark.parametrize("dot_command", [
    ".mode csv",
    ""
])
def test_mode_csv(shell, dot_command):
    args = ['-csv'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT NULL, 42, 'fourty-two', 42.0;")
    )
    result = test.run()
    result.check_stdout(',fourty-two,')

@pytest.mark.parametrize("dot_command", [
    ".mode column",
    ""
])
def test_mode_column(shell, dot_command):
    args = ['-column'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT NULL, 42, 'fourty-two', 42.0;")
    )
    result = test.run()
    result.check_stdout('  fourty-two  ')

def test_mode_html(shell):
    test = (
        ShellTest(shell)
        .statement(".mode html")
        .statement("SELECT NULL, 42, 'fourty-two', 42.0;")
    )
    result = test.run()
    result.check_stdout('<td>fourty-two</td>')

@pytest.mark.parametrize("dot_command", [
    ".mode html",
    ""
])
def test_mode_html_escapes(shell, dot_command):
    args = ['-html'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT '<&>\"\'\'' AS \"&><\"\"\'\";")
    )
    result = test.run()
    result.check_stdout('<tr><th>&amp;&gt;&lt;&quot;&#39;</th>')

def test_mode_tcl_escapes(shell):
    test = (
        ShellTest(shell)
        .statement(".mode tcl")
        .statement("SELECT '<&>\"\'\'' AS \"&><\"\"\'\";")
    )
    result = test.run()
    result.check_stdout('"&><\\"\'"')

def test_mode_csv_escapes(shell):
    test = (
        ShellTest(shell)
        .statement(".mode csv")
        .statement("SELECT 'BEGINVAL,\n\"ENDVAL' AS \"BEGINHEADER\"\",\nENDHEADER\";")
    )
    result = test.run()
    result.check_stdout('"BEGINHEADER"",\nENDHEADER"\r\n"BEGINVAL,\n""ENDVAL"')


@pytest.mark.parametrize("dot_command", [
    ".mode json",
    ""
])
def test_mode_json_infinity(shell, dot_command):
    args = ['-json'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT 'inf'::DOUBLE AS inf, '-inf'::DOUBLE AS ninf, 'nan'::DOUBLE AS nan, '-nan'::DOUBLE AS nnan;")
    )
    result = test.run()
    result.check_stdout('[{"inf":Infinity,"ninf":-Infinity,"nan":NaN,"nnan":NaN}]')

def test_mode_insert(shell):
    test = (
        ShellTest(shell)
        .statement(".mode insert")
        .statement("SELECT NULL, 42, 'fourty-two', 42.0, 3.14, 2.71;")
    )
    result = test.run()
    result.check_stdout('fourty-two')
    result.check_stdout('3.14')
    result.check_stdout('2.71')
    result.check_not_exist('3.140000')
    result.check_not_exist('2.709999')
    result.check_not_exist('3.139999')
    result.check_not_exist('2.710000')

def test_mode_insert_table(shell):
    test = (
        ShellTest(shell)
        .statement(".mode insert my_table")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stdout('my_table')

@pytest.mark.parametrize("dot_command", [
    ".mode line",
    ""
])
def test_mode_line(shell, dot_command):
    args = ['-line'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT NULL, 42, 'fourty-two' x, 42.0;")
    )
    result = test.run()
    result.check_stdout('x = fourty-two')

@pytest.mark.parametrize("dot_command", [
    ".mode list",
    ""
])
def test_mode_list(shell, dot_command):
    args = ['-list'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT NULL, 42, 'fourty-two' x, 42.0;")
    )
    result = test.run()
    result.check_stdout('|fourty-two|')

# Original comment: FIXME sqlite3_column_blob and %! format specifier

@pytest.mark.parametrize("dot_command", [
    ".mode quote",
    ""
])
def test_mode_quote(shell, dot_command):
    args = ['-quote'] if len(dot_command) == 0 else []
    test = (
        ShellTest(shell, args)
        .statement(dot_command)
        .statement("SELECT NULL, 42, 'fourty-two' x, 42.0;")
    )
    result = test.run()
    result.check_stdout('fourty-two')

def test_mode_tabs(shell):
    test = (
        ShellTest(shell)
        .statement(".mode tabs")
        .statement("SELECT NULL, 42, 'fourty-two' x, 42.0;")
    )
    result = test.run()
    result.check_stdout('fourty-two')

def test_open(shell, tmp_path):
    file_one = tmp_path / "file_one"
    file_two = tmp_path / "file_two"
    test = (
        ShellTest(shell)
        .statement(f".open {file_one.as_posix()}")
        .statement("CREATE TABLE t1 (i INTEGER);")
        .statement("INSERT INTO t1 VALUES (42);")
        .statement(f".open {file_two.as_posix()}")
        .statement("CREATE TABLE t2 (i INTEGER);")
        .statement("INSERT INTO t2 VALUES (43);")
        .statement(f".open {file_one.as_posix()}")
        .statement("SELECT * FROM t1;")
    )
    result = test.run()
    result.check_stdout('42')

@pytest.mark.parametrize('generated_file', ["blablabla"], indirect=True)
def test_open_non_database(shell, generated_file):
    test = (
        ShellTest(shell)
        .add_argument(generated_file.as_posix())
    )
    result = test.run()
    result.check_stderr('not a valid DuckDB database file')

def test_enable_profiling(shell):
    test = (
        ShellTest(shell)
        .statement("PRAGMA enable_profiling")
    )
    result = test.run()
    result.check_stderr(None)

def test_profiling_select(shell):
    test = (
        ShellTest(shell)
        .statement("PRAGMA enable_profiling")
        .statement("select 42")
    )
    result = test.run()
    result.check_stderr('Total Time')
    result.check_stdout('42')

@pytest.mark.skipif(os.name == 'nt', reason="echo does not exist on Windows")
@pytest.mark.parametrize("command", [
    "system",
    "shell"
])
def test_echo_command(shell, command):
    test = (
        ShellTest(shell)
        .statement(f".{command} echo 42")
    )
    result = test.run()
    result.check_stdout('42')

def test_system_pwd_command(shell):
    test = (
        ShellTest(shell)
        .statement(f".sh pwd")
    )
    result = test.run()
    result.check_stdout('duckdb')

def test_profiling_optimizer(shell):
    test = (
        ShellTest(shell)
        .statement("PRAGMA enable_profiling=query_tree_optimizer;")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stderr('Total Time')
    result.check_stdout('42')

def test_profiling_optimizer_detailed(shell):
    test = (
        ShellTest(shell)
        .statement("PRAGMA enable_profiling;")
        .statement("PRAGMA profiling_mode=detailed;")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stderr('Total Time')
    result.check_stdout('42')

def test_profiling_optimizer_json(shell):
    test = (
        ShellTest(shell)
        .statement("PRAGMA enable_profiling=json;")
        .statement("PRAGMA profiling_mode=detailed;")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stderr('optimizer')
    result.check_stdout('42')


# Original comment: this fails because db_config is missing
@pytest.mark.skip(reason="db_config is not supported (yet?)")
def test_eqp(shell):
    test = (
        ShellTest(shell)
        .statement(".eqp full")
        .statement("SELECT 42;")
    )
    result = test.run()
    result.check_stdout('DUMMY_SCAN')

def test_databases(shell):
    test = (
        ShellTest(shell)
        .statement("ATTACH ':memory:' AS xx")
        .statement(".databases")
    )
    result = test.run()
    result.check_stdout('memory')
    result.check_stdout('xx')

def test_invalid_csv(shell, tmp_path):
    # import ignores errors
    file = tmp_path / 'nonsencsv.csv'
    with open(file, 'wb+') as f:
        f.write(b'\xFF\n42\n')
    test = (
        ShellTest(shell)
        .statement(".nullvalue NULL")
        .statement("CREATE TABLE test(i INTEGER);")
        .statement(f".import {file.as_posix()} test")
        .statement("SELECT * FROM test;")
    )
    result = test.run()
    result.check_stdout('42')

def test_mode_latex(shell):
    test = (
        ShellTest(shell)
        .statement(".mode latex")
        .statement("CREATE TABLE a (I INTEGER);")
        .statement(".changes off")
        .statement("INSERT INTO a VALUES (42);")
        .statement("SELECT * FROM a;")
    )
    result = test.run()
    result.check_stdout('\\begin{tabular}')

def test_mode_trash(shell):
    test = (
        ShellTest(shell)
        .statement(".mode trash")
        .statement("select 1")
    )
    result = test.run()
    result.check_stdout(None)

def test_mode_trash_runs_to_completion(shell):
    # the rows are discarded, but the query must still run to completion
    test = (
        ShellTest(shell)
        .statement("CREATE SEQUENCE seq")
        .statement(".mode trash")
        .statement("SELECT nextval('seq') FROM range(1000000)")
        .statement(".mode csv")
        .statement("SELECT currval('seq')")
    )
    result = test.run()
    result.check_stdout("1000000")

def test_sqlite_comments(shell):
    # Using /* <comment> */
    test = (
        ShellTest(shell)
        .statement("""/*
;
*/""")
        .statement("select 42")
    )
    result = test.run()
    result.check_stdout('42')

    # Using -- <comment>
    test = (
        ShellTest(shell)
        .statement("""-- this is a comment ;
select 42;
""")
    )
    result = test.run()
    result.check_stdout('42')

    # More extreme: -- <comment>
    test = (
        ShellTest(shell)
        .statement("""--;;;;;;
select 42;
""")
    )
    result = test.run()
    result.check_stdout('42')

    # More extreme: /* <comment> */
    test = (
        ShellTest(shell)
        .statement('/* ;;;;;; */ select 42;')
    )
    result = test.run()
    result.check_stdout('42')

def test_duckbox(shell):
    test = (
        ShellTest(shell)
        .statement(".mode duckbox")
        .statement("select 42 limit 0;")
    )
    result = test.run()
    result.check_stdout('0 rows')

def test_duckbox_enum_type_rendering(shell):
    test = (
        ShellTest(shell)
        .statement(".mode duckbox")
        .statement("SELECT 'A'::ENUM('A','a') AS upper_a, 'A'::ENUM('a','A') AS lower_a LIMIT 0")
    )
    result = test.run()
    result.check_stdout("enum('A', 'a')")
    result.check_stdout("enum('a', 'A')")
    result.check_not_exist("enum('a', 'a')")

def test_duckbox_malformed_json(shell):
    test = (
        ShellTest(shell)
        .statement(".mode duckbox")
        .statement("select union_value(\"c1\" := '}');")
    )
    result = test.run()
    result.check_stdout('}')

    # nested object
    test = (
        ShellTest(shell)
        .statement(".mode duckbox")
        .statement("select union_value(\"c1\" := '[\"a\", {]');")
    )
    result = test.run()
    result.check_stdout('[\"a\", {]')

# Original comment: #5411 - with maxrows=2, we still display all 4 rows (hiding them would take up more space)
def test_maxrows(shell):
    test = (
        ShellTest(shell)
        .statement(".maxrows 2")
        .statement("select * from range(4);")
    )
    result = test.run()
    result.check_stdout('1')

def test_maxrows_outfile(shell, random_filepath):
    file = random_filepath
    test = (
        ShellTest(shell)
        .statement(".maxrows 2")
        .statement(f".output {file.as_posix()}")
        .statement("SELECT * FROM range(100);")
    )
    result = test.run()
    result.stdout = open(file, 'rb').read().decode('utf8')
    result.check_stdout('50')

def test_columns_to_file(shell, random_filepath):
    columns = ', '.join([str(x) for x in range(100)])
    test = (
        ShellTest(shell)
        .statement(f".output {random_filepath.as_posix()}")
        .statement(f"SELECT {columns}")
    )
    result = test.run()
    result.stdout = open(random_filepath, 'rb').read().decode('utf8')
    result.check_stdout('99')

def test_columnar_mode(shell):
    test = (
        ShellTest(shell)
        .statement(".col")
        .statement("select * from range(4);")
    )
    result = test.run()
    result.check_stdout('Row 1')

def test_columnar_mode_truncate(shell):
    test = (
        ShellTest(shell)
        .statement(".col")
        .statement(".maxwidth 100")
        .statement("select * from range(100,200);")
    )
    result = test.run()
    result.check_stdout('Row 98')
    result.check_stdout('198')

def test_empty_result(shell):
    test = (
        ShellTest(shell)
        .statement("select 42 empty_result where 1=0;")
    )
    result = test.run()
    result.check_stdout('''┌──────────────┐
│ empty_result │
│    int32     │
└──────────────┘
     0 rows''')

def test_columnar_mode_constant(shell):
    columns = ','.join(["'MyValue" + str(x) + "'" for x in range(100)])
    test = (
        ShellTest(shell)
        .statement(".col")
        .statement(f"select {columns};")
    )
    result = test.run()
    result.check_stdout('MyValue50')

    test = (
        ShellTest(shell)
        .statement(".col")
        .statement(f"select {columns} from range(1000);")
    )
    result = test.run()
    result.check_stdout('100 columns')

def test_nullbyte_rendering(shell):
    test = (
        ShellTest(shell)
        .statement("select varchar from test_all_types();")
    )
    result = test.run()
    result.check_stdout('goo\\0se')

def test_nullbyte_error_rendering(shell):
    test = (
        ShellTest(shell)
        .statement("select chr(0)::int")
    )
    result = test.run()
    result.check_stderr('INT32')

def test_thousand_sep(shell):
    test = (
        ShellTest(shell)
        .statement(".thousand_sep space")
        .statement("SELECT 10000")
        .statement(".thousand_sep ,")
        .statement("SELECT 10000")
        .statement(".thousand_sep none")
        .statement("SELECT 10000")
        .statement(".thousand_sep")
    )

    result = test.run()
    result.check_stdout("10 000")
    result.check_stdout("10,000")
    result.check_stdout("10000")
    result.check_stdout("current thousand separator")

def test_decimal_sep(shell):
    test = (
        ShellTest(shell)
        .statement(".decimal_sep space")
        .statement("SELECT 10.5")
        .statement(".decimal_sep ,")
        .statement("SELECT 10.5")
        .statement(".decimal_sep none")
        .statement("SELECT 10.5")
        .statement(".decimal_sep")
    )

    result = test.run()
    result.check_stdout("10 5")
    result.check_stdout("10,5")
    result.check_stdout("10.5")
    result.check_stdout("current decimal separator")

def test_prepared_statement(shell):
    test = ShellTest(shell).statement("select ?")
    result = test.run()
    result.check_stderr("Prepared statement parameters cannot be used directly")

def test_shell_csv_file(shell):
    test = (
        ShellTest(shell, ['data/csv/dates.csv'])
        .statement('SELECT * FROM dates')
    )
    result = test.run()
    result.check_stdout("2008-08-10")

def test_tables_invalid_pattern_handling(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE test_table(i INTEGER);")
        .statement(".tables \"invalid\"pattern\"")
    )
    result = test.run()
    # Should show usage message for invalid pattern
    result.check_stderr("Usage")

def test_help_prints_to_stdout(shell):
    test = ShellTest(shell, ["--help"])
    result = test.run()
    result.check_stdout("OPTIONS")

def test_open_with_sql(shell, random_filepath):
    test = (
        ShellTest(shell)
        .env_var("MY_DB", str(random_filepath))
        .statement(".open -sql \"select getenv('MY_DB');\"")
        .statement("show databases;")
    )
    result = test.run()
    result.check_stdout("random_import_file")
    result.check_stderr(None)

def test_open_with_multiple_sql_flags(shell, random_filepath):
    test = (
        ShellTest(shell)
        .env_var("MY_DB", str(random_filepath))
        .statement(".open -sql \"select getenv('MY_DB');\" -sql \"select 42;\"")
    )
    result = test.run()
    result.check_stderr("Error: --sql provided multiple times")

def test_open_with_sql_and_no_query(shell, random_filepath):
    test = (
        ShellTest(shell)
        .env_var("MY_DB", str(random_filepath))
        .statement(".open -sql")
    )
    result = test.run()
    result.check_stderr("Error: missing SQL query after --sql")

def test_open_with_sql_and_file(shell, random_filepath):
    test = (
        ShellTest(shell)
        .env_var("MY_DB", str(random_filepath))
        .statement(".open -sql \"select getenv('MY_DB');\" \"test.db\"")
    )
    result = test.run()
    result.check_stderr("Error: cannot use both --sql and a FILE argument")

def test_open_with_sql_and_multiple_columns(shell, random_filepath):
    test = (
        ShellTest(shell)
        .env_var("MY_DB", str(random_filepath))
        .statement(".open -sql \"select getenv('MY_DB'), 42;\"")
    )
    result = test.run()
    result.check_stderr("Error: --sql query returned multiple columns, expected single value")

def test_open_with_sql_and_multiple_rows(shell):
    test = (
        ShellTest(shell)
        .statement(".open -sql \"select unnest(generate_series(1,2));\"")
    )
    result = test.run()
    result.check_stderr("Error: --sql query returned multiple rows, expected single value")

def test_open_with_sql_w_db_error(shell):
    test = (
        ShellTest(shell)
        .statement(".open -sql \"select 'test'::int;\"")
    )
    result = test.run()
    result.check_stderr("Error: failed to evaluate --sql query")

def test_open_with_sql_and_no_return(shell):
    test = (
        ShellTest(shell)
        .statement("create table a (i integer);")
        .statement(".open -sql \"from a where 1=0;\"")
    )
    result = test.run()
    result.check_stderr("Error")

def test_open_with_sql_and_dml(shell):
    test = (
        ShellTest(shell)
        .statement("create table test(i integer);")
        .statement(".open -sql \"insert into test values (1);\"")
    )
    result = test.run()
    result.check_stderr("Error")

def test_open_with_sql_and_null_return(shell):
    test = (
        ShellTest(shell)
        .statement(".open -sql \"select NULL;\"")
    )
    result = test.run()
    result.check_stderr("Error: --sql query returned a null value")


def test_about(shell):
    test = ShellTest(shell).statement(".about")

    result = test.run()
    result.check_stdout("DuckDB is an in-process analytical database management system designed for fast ")


def test_a_select_that_writes_keeps_its_rows_when_the_renderer_stops_early(shell):
    # agent mode's renderer stops reading after a cap of rows; the insert must not be undone by that
    test = (
        ShellTest(shell)
        .add_argument("-agent")
        .statement("create table t(i bigint)")
        .statement("with ins as (insert into t select range from range(200000) returning i) select * from ins")
        .statement("select count(*) as c from t")
    )
    result = test.run()
    result.check_stdout("| 200000 |")


def run_shell(shell, arguments, sql, timeout=120):
    # the same environment the ShellTest harness builds, so an agent running the tests does not switch on agent mode
    environment = os.environ.copy()
    for key in ShellTest.AGENT_ENVIRONMENT_VARIABLES:
        environment.pop(key, None)
    return subprocess.run(
        [shell, "--batch", "--no-init"] + arguments + ["-c", sql],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=environment,
    )


def test_a_select_that_writes_keeps_its_rows_when_the_output_pipe_closes(shell, tmp_path):
    database = (tmp_path / "pipe.db").as_posix()
    created = run_shell(shell, [database], "create table t(i bigint)")
    assert created.returncode == 0, created.stderr
    insert = "with ins as (insert into t select range from range(200000) returning i) select * from ins"
    environment = os.environ.copy()
    for key in ShellTest.AGENT_ENVIRONMENT_VARIABLES:
        environment.pop(key, None)
    reader = subprocess.Popen(
        [shell, "--batch", "--no-init", "-csv", database, "-c", insert],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    try:
        # stop reading after the header, as `| head -1` would, then let the closed pipe stop the shell
        assert reader.stdout.readline().strip() == "i"
        reader.stdout.close()
        reader.stdout = None
        _, stderr = reader.communicate(timeout=120)
    finally:
        reader.kill()
        reader.wait(timeout=60)
    counted = run_shell(shell, [database, "-csv", "-noheader"], "select count(*) from t")
    assert counted.returncode == 0, counted.stderr
    assert counted.stdout.strip() == "200000", stderr


def test_dynamic_pivot(shell):
    test = (
        ShellTest(shell)
        .statement(".mode list")
        .statement("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER);")
        .statement("INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20), ('rtm', 2023, 30);")
        .statement("PIVOT sales ON year USING sum(amount) ORDER BY city;")
        .statement("BEGIN; PIVOT sales ON year USING sum(amount) ORDER BY city DESC; COMMIT;")
    )
    result = test.run()
    result.check_stdout(["city|2023|2024", "ams|10|20", "rtm|30|NULL", "city|2023|2024", "rtm|30|NULL", "ams|10|20"])


def test_alter_with_volatile_default(shell):
    test = (
        ShellTest(shell)
        .statement(".mode list")
        .statement("CREATE TABLE t AS SELECT range AS i FROM range(1000);")
        .statement("ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random();")
        .statement("SELECT count(*) AS filled FROM t WHERE c IS NOT NULL;")
    )
    result = test.run()
    result.check_stdout("filled\n1000")


def test_failing_alter_leaves_no_column(shell):
    test = (
        ShellTest(shell)
        .statement(".mode list")
        .statement("CREATE TABLE t AS SELECT range AS i FROM range(1000);")
        .statement("ALTER TABLE t ADD COLUMN d INTEGER DEFAULT nonexistent_function();")
        .statement("SELECT count(*) AS columns FROM duckdb_columns() WHERE table_name = 't';")
    )
    result = test.run()
    result.check_stderr("nonexistent_function")
    assert result.stdout.splitlines()[-2:] == ["columns", "1"]


def test_explain_dynamic_pivot_renders_the_plan(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER);")
        .statement("INSERT INTO sales VALUES ('ams', 2023, 10);")
        .statement("EXPLAIN PIVOT sales ON year USING sum(amount);")
    )
    result = test.run()
    result.check_stdout("Seq Scan")
    result.check_not_exist("explain_key")


def test_describe_dynamic_pivot_renders_the_description(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER);")
        .statement("INSERT INTO sales VALUES ('ams', 2023, 10);")
        .statement("DESCRIBE PIVOT sales ON year USING sum(amount);")
    )
    result = test.run()
    result.check_stdout("city")
    result.check_not_exist("column_type")

# fmt: on
