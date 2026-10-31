# fmt: off

from conftest import ShellTest

# tests for the statement-completeness scanner (ShellState::SQLIsComplete)
# routine bodies framed by BEGIN ... END may contain semicolons - the shell must submit them as one statement


def run_lines(shell, text):
    return ShellTest(shell).run_raw(text)


class TestBeginEndBodies(object):
    def test_function_body_single_submission(self, shell):
        result = run_lines(
            shell,
            'CREATE OR REPLACE FUNCTION f(n INTEGER) RETURNS INTEGER AS BEGIN\n'
            '  SELECT 1;\n'
            '  RETURN n;\n'
            'END;\n'
            '.print CANARY\n'
            "SELECT 'done' AS marker;\n",
        )
        # the body is submitted as a single statement - one parser error, not one per fragment
        assert result.stderr.count("Parser Error") == 1
        # the closing END is not executed as a transaction COMMIT
        assert "cannot commit" not in result.stderr
        # the buffer is released after the statement, so the dot command runs
        assert "CANARY" in result.stdout
        assert "done" in result.stdout

    def test_function_body_nested_blocks(self, shell):
        result = run_lines(
            shell,
            'CREATE FUNCTION h(n INTEGER) RETURNS INTEGER AS BEGIN\n'
            '  IF n > 0 THEN\n'
            '    RETURN 1;\n'
            '  END;\n'
            '  BEGIN\n'
            '    SELECT 2;\n'
            '  END;\n'
            '  RETURN 0;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "CANARY" in result.stdout

    def test_procedure_body_end_loop(self, shell):
        # PL/pgSQL-style blocks closed with END LOOP / END IF stay balanced
        result = run_lines(
            shell,
            'CREATE PROCEDURE p() AS BEGIN\n'
            '  WHILE x < 10 LOOP\n'
            '    SET x = x + 1;\n'
            '  END LOOP;\n'
            '  IF x > 5 THEN\n'
            '    SELECT x;\n'
            '  END IF;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "CANARY" in result.stdout

    def test_trigger_body_sqlite_style(self, shell):
        # sqlite trigger syntax has no AS before BEGIN
        result = run_lines(
            shell,
            'CREATE TRIGGER tr AFTER INSERT ON t BEGIN\n'
            '  UPDATE t SET i = 1;\n'
            '  DELETE FROM t;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "CANARY" in result.stdout

    def test_function_body_case_strings_comments(self, shell):
        # CASE ... END expressions, strings and comments containing END; do not unbalance the body
        result = run_lines(
            shell,
            'CREATE FUNCTION f2(n INTEGER) RETURNS INTEGER AS BEGIN\n'
            "  LET s := 'END; not a real end';\n"
            '  -- comment with END; inside\n'
            '  RETURN CASE WHEN n > 0 THEN 1 ELSE 0 END;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "CANARY" in result.stdout

    def test_transaction_statements_inside_body(self, shell):
        # BEGIN TRANSACTION / BEGIN; inside a body do not open nested blocks
        result = run_lines(
            shell,
            'CREATE FUNCTION f3() RETURNS INTEGER AS BEGIN\n'
            '  BEGIN TRANSACTION;\n'
            '  RETURN 1;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "CANARY" in result.stdout


class TestBeginEndProtection(object):
    # existing statement splitting behavior that must not change

    def test_toplevel_transactions(self, shell):
        result = run_lines(
            shell,
            'BEGIN TRANSACTION;\n'
            'CREATE TABLE t(i INTEGER);\n'
            'INSERT INTO t VALUES (42);\n'
            'COMMIT;\n'
            'BEGIN;\n'
            'INSERT INTO t VALUES (43);\n'
            'END;\n'
            'SELECT count(*) AS cnt FROM t;\n',
        )
        result.check_stdout("cnt")
        result.check_stdout("2")

    def test_begin_as_alias(self, shell):
        # BEGIN is an unreserved keyword and stays usable as an alias
        result = run_lines(
            shell,
            'SELECT 1 AS begin;\n'
            '.print CANARY\n',
        )
        result.check_stdout("begin")
        result.check_stdout("CANARY")

    def test_create_function_macro(self, shell):
        # CREATE FUNCTION as a macro alias without a block body is unaffected
        result = run_lines(
            shell,
            'CREATE FUNCTION g(a) AS a + 1;\n'
            'SELECT g(1) AS gval;\n'
            '.print CANARY\n',
        )
        result.check_stdout("gval")
        result.check_stdout("2")
        result.check_stdout("CANARY")

    def test_toplevel_case_end(self, shell):
        result = run_lines(
            shell,
            "SELECT CASE WHEN 1=1 THEN 'yes' ELSE 'no' END AS c;\n"
            '.print CANARY\n',
        )
        result.check_stdout("yes")
        result.check_stdout("CANARY")

    def test_dollar_quoted_semicolons(self, shell):
        result = run_lines(
            shell,
            'SELECT $$a; b$$ AS s;\n'
            '.print CANARY\n',
        )
        result.check_stdout("a; b")
        result.check_stdout("CANARY")
