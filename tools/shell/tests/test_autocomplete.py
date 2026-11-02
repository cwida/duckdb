# fmt: off

import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest
from conftest import autocomplete_extension
import os

# 'autocomplete_extension' is a fixture which will skip the test if 'autocomplete' is not loaded
def test_autocomplete_select(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CALL sql_auto_complete('SEL')")
    )
    result = test.run()
    result.check_stdout('SELECT')

def test_autocomplete_first_from(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CALL sql_auto_complete('FRO')")
    )
    result = test.run()
    result.check_stdout('FROM')

def test_autocomplete_column(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('SELECT my_') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('my_column')

def test_autocomplete_where(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('SELECT my_column FROM my_table WH') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('WHERE')

def test_autocomplete_insert(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('INS') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('INSERT')

def test_autocomplete_into(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('INSERT IN') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('INTO')

def test_autocomplete_into_table(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('INSERT INTO my_t') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('my_table')

def test_autocomplete_values(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('INSERT INTO my_table VAL') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('VALUES')

def test_autocomplete_delete(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('DEL') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('DELETE')

def test_autocomplete_delete_from(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('DELETE F') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('FROM')

def test_autocomplete_from_table(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('DELETE FROM m') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('my_table')

def test_autocomplete_update(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('UP') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('UPDATE')

def test_autocomplete_update_table(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('UPDATE m') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('my_table')

    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("""SELECT * FROM sql_auto_complete('UPDATE "m') LIMIT 1;""")
    )
    result = test.run()
    result.check_stdout('my_table')

def test_autocomplete_update_column(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE my_table(my_column INTEGER);")
        .statement("SELECT * FROM sql_auto_complete('UPDATE my_table SET m') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('my_column')

def test_autocomplete_funky_table(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("""CREATE TABLE "Funky Table With Spaces"(my_column INTEGER);""")
        .statement("SELECT suggestion FROM sql_auto_complete('SELECT * FROM F') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('"Funky Table With Spaces"')

    test = (
        ShellTest(shell)
        .statement("""CREATE TABLE "Funky Table With Spaces"("Funky Column" int);""")
        .statement("""SELECT suggestion FROM sql_auto_complete('select "Funky Column" FROM f') LIMIT 1;""")
    )
    result = test.run()
    result.check_stdout('"Funky Table With Spaces"')

def test_autocomplete_funky_column(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("""CREATE TABLE "Funky Table With Spaces"("Funky Column" int);""")
        .statement("SELECT * FROM sql_auto_complete('select f') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('"Funky Column"')

def test_autocomplete_semicolon(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("SELECT * FROM sql_auto_complete('SELECT 42; SEL') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('SELECT')

def test_autocomplete_comments(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("""
SELECT * FROM sql_auto_complete('--SELECT * FROM
SEL') LIMIT 1;""")
    )
    result = test.run()
    result.check_stdout('SELECT')

def test_autocomplete_scalar_functions(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("SELECT * FROM sql_auto_complete('SELECT regexp_m') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('regexp_matches')

def test_autocomplete_aggregates(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("SELECT * FROM sql_auto_complete('SELECT approx_c') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('approx_count_distinct')

def test_autocomplete_builtin_views(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("SELECT * FROM sql_auto_complete('SELECT * FROM sqlite_ma') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('sqlite_master')

def test_autocomplete_table_function(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("SELECT * FROM sql_auto_complete('SELECT * FROM read_csv_a') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('read_csv_auto')

def test_autocomplete_tpch(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE partsupp(ps_suppkey int);")
        .statement("CREATE TABLE supplier(s_suppkey int);")
        .statement("CREATE TABLE nation(n_nationkey int);")
        .statement("SELECT * FROM sql_auto_complete('DROP TABLE na') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('nation')

    test = (
        ShellTest(shell)
        .statement("CREATE TABLE partsupp(ps_suppkey int);")
        .statement("CREATE TABLE supplier(s_suppkey int);")
        .statement("CREATE TABLE nation(n_nationkey int);")
        .statement("SELECT * FROM sql_auto_complete('SELECT s_supp') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('s_suppkey')

    test = (
        ShellTest(shell)
        .statement("CREATE TABLE partsupp(ps_suppkey int);")
        .statement("CREATE TABLE supplier(s_suppkey int);")
        .statement("CREATE TABLE nation(n_nationkey int);")
        .statement("SELECT * FROM sql_auto_complete('SELECT * FROM partsupp JOIN supp') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('supplier')

    test = (
        ShellTest(shell)
        .statement("CREATE TABLE partsupp(ps_suppkey int);")
        .statement("CREATE TABLE supplier(s_suppkey int);")
        .statement("CREATE TABLE nation(n_nationkey int);")
        .statement(".mode csv")
        .statement("SELECT l,l FROM sql_auto_complete('SELECT * FROM partsupp JOIN supplier ON (s_supp') t(l) LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('s_suppkey,s_suppkey')

    test = (
        ShellTest(shell)
        .statement("CREATE TABLE partsupp(ps_suppkey int);")
        .statement("CREATE TABLE supplier(s_suppkey int);")
        .statement("CREATE TABLE nation(n_nationkey int);")
        .statement("SELECT * FROM sql_auto_complete('SELECT * FROM partsupp JOIN supplier USING (ps_su') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('ps_suppkey')

def test_autocomplete_from(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("SELECT * FROM sql_auto_complete('SELECT * FR') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('FROM')

def test_autocomplete_disambiguation_column(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE MyTable(MyColumn Varchar);")
        .statement("SELECT * FROM sql_auto_complete('SELECT My') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('MyColumn')
    
def test_autocomplete_disambiguation_table(shell, autocomplete_extension):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE MyTable(MyColumn Varchar);")
        .statement("SELECT * FROM sql_auto_complete('SELECT MyColumn FROM My') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout('MyTable')

def test_autocomplete_directory(shell, autocomplete_extension, tmp_path):
    shell_test_dir = tmp_path / 'shell_test_dir'
    extra_path = tmp_path / 'shell_test_dir' / 'extra_path'
    shell_test_dir.mkdir()
    extra_path.mkdir()

    # Create the files
    base_files = ['extra.parquet', 'extra.file']
    for fname in base_files:
        with open(shell_test_dir / fname, 'w+') as f:
            f.write('')

    # Complete the directory
    partial_directory = tmp_path / 'shell_test'
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE MyTable(MyColumn Varchar);")
        .statement(f"SELECT * FROM sql_auto_complete('SELECT * FROM ''{partial_directory.as_posix()}') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout("shell_test_dir")

    # Complete the sub directory as well
    partial_subdirectory = tmp_path / 'shell_test_dir' / 'extra'
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE MyTable(MyColumn Varchar);")
        .statement(f"SELECT * FROM sql_auto_complete('SELECT * FROM ''{partial_subdirectory.as_posix()}') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout("extra_path")

    # Complete the parquet file in the sub directory
    partial_parquet = tmp_path / 'shell_test_dir' / 'extra.par'
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE MyTable(MyColumn Varchar);")
        .statement(f"SELECT * FROM sql_auto_complete('SELECT * FROM ''{partial_parquet.as_posix()}') LIMIT 1;")
    )
    result = test.run()
    result.check_stdout("extra.parquet")


# Ties between completions complete up to their longest common prefix. The completions cover the
# text before the cursor only, so the first <tab> must keep the characters after the cursor: text
# typed after it must not end up where the trailing quote and semicolon used to be. See #27985.
@pytest.mark.skipif(os.name == "nt", reason="requires a POSIX pseudo-terminal")
def test_tab_completion_preserves_text_after_cursor(shell, tmp_path, autocomplete_extension):
    import fcntl
    import pty
    import select
    import struct
    import termios
    import time

    for name in ("table_one.parquet", "table_two.parquet", "table_three.parquet"):
        (tmp_path / name).touch()
    history = tmp_path / "history"
    env = ShellTest(shell).get_environment()
    env["TERM"] = "xterm-256color"
    # `shell` may be a relative path; spawn() below changes the working directory,
    # so resolve it against the repository root before starting the process.
    binary = os.path.abspath(shell)
    env["DUCKDB_HISTORY"] = str(history)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    process = None

    def drain(seconds):
        # Answer the terminal capability queries that the line editor waits for.
        end = time.time() + seconds
        while time.time() < end:
            readable, _, _ = select.select([master], [], [], 0.05)
            if master not in readable:
                continue
            try:
                output = os.read(master, 65536)
            except OSError:
                return
            if b"\x1b]11;?" in output:
                os.write(master, b"\x1b]11;rgb:0000/0000/0000\x07")
            if b"\x1b[c" in output:
                os.write(master, b"\x1b[?1;2c")
            if b"\x1b[6n" in output:
                os.write(master, b"\x1b[1;1R")

    try:
        process = subprocess.Popen(
            [binary, "-no-init", "-interactive"],
            cwd=tmp_path,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=env,
        )
        os.close(slave)
        slave = None
        drain(2.0)
        os.write(master, b"describe from 'ta")
        drain(0.2)
        os.write(master, b"';")
        drain(0.2)
        # Move the cursor in front of the quote so the quote and semicolon follow the cursor.
        os.write(master, b"\x1b[D\x1b[D")
        drain(0.2)
        # The first <tab> applies the longest common prefix of the file completions.
        os.write(master, b"\t")
        drain(1.0)
        # Finish the file name without another <tab> and submit the line.
        os.write(master, b"two.parquet")
        drain(0.3)
        os.write(master, b"\r")
        drain(0.5)
        # Canceling also ends the pending statement if the completion lost the semicolon.
        os.write(master, b"\x03")
        drain(0.5)
        os.write(master, b".exit\r")
        drain(1.0)
        process.wait(timeout=10)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if slave is not None:
            os.close(slave)
        os.close(master)

    assert history.is_file()
    saved = history.read_text(encoding="utf-8")
    assert "describe from 'table_two.parquet';" in saved, saved


# fmt: on
