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

    def test_nested_block_after_loop(self, shell):
        # a nested block directly after LOOP is still part of the body
        result = run_lines(
            shell,
            'CREATE PROCEDURE p() AS BEGIN\n'
            '  LOOP\n'
            '    BEGIN\n'
            '      SELECT 1;\n'
            '    END;\n'
            '  END LOOP;\n'
            "  SELECT 'escaped' AS marker;\n"
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "escaped" not in result.stdout
        assert "CANARY" in result.stdout

    def test_nested_if_after_then(self, shell):
        # a nested IF directly after THEN is still part of the body
        result = run_lines(
            shell,
            'CREATE FUNCTION f4(n INTEGER) RETURNS INTEGER AS BEGIN\n'
            '  IF n > 0 THEN IF n > 1 THEN\n'
            '    RETURN 2;\n'
            '  END;\n'
            '  END;\n'
            '  LOOP END LOOP;\n'
            '  RETURN 0;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "CANARY" in result.stdout

    def test_case_expressions_inside_body(self, shell):
        # CASE expression keywords inside a body are not procedural - qualified columns named
        # loop/then/else, nested CASE expressions and if() calls do not change the block depth
        result = run_lines(
            shell,
            'CREATE PROCEDURE p() AS BEGIN\n'
            '  SELECT CASE WHEN true THEN t.loop END FROM (VALUES (1)) t(loop);\n'
            '  SELECT CASE WHEN true THEN CASE WHEN true THEN 1 ELSE 2 END ELSE 3 END;\n'
            '  SELECT CASE WHEN true THEN if(true, 1, 2) ELSE 3 END;\n'
            "  SELECT 'escaped' AS marker;\n"
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "escaped" not in result.stdout
        assert "CANARY" in result.stdout

    def test_multibyte_column_with_keyword_suffix_inside_body(self, shell):
        # a multi-byte identifier ending in a keyword is one word, not that keyword
        result = run_lines(
            shell,
            'CREATE PROCEDURE p() AS BEGIN\n'
            '  SELECT CASE WHEN true THEN t.éloop END FROM (VALUES (1)) t(éloop);\n'
            "  SELECT 'escaped' AS marker;\n"
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "escaped" not in result.stdout
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
        # transaction statements inside a body do not open or close blocks
        result = run_lines(
            shell,
            'CREATE FUNCTION f3() RETURNS INTEGER AS BEGIN\n'
            '  BEGIN TRANSACTION;\n'
            '  END TRANSACTION;\n'
            '  BEGIN WORK;\n'
            '  END WORK;\n'
            '  BEGIN READ ONLY;\n'
            '  COMMIT;\n'
            '  BEGIN READ WRITE;\n'
            '  COMMIT;\n'
            '  BEGIN;\n'
            '  ROLLBACK;\n'
            "  SELECT 'still inside' AS marker;\n"
            '  RETURN 1;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "cannot commit" not in result.stderr
        assert "still inside" not in result.stdout
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

    def test_macro_with_begin_alias(self, shell):
        # a begin alias or parameter after a later AS inside a valid macro does not open body mode
        result = run_lines(
            shell,
            'CREATE FUNCTION m1() AS TABLE SELECT 1 AS begin;\n'
            'SELECT * FROM m1();\n'
            'CREATE FUNCTION m2() AS (SELECT 2 AS begin);\n'
            'SELECT m2() AS v;\n'
            'CREATE FUNCTION m3() AS 0, (begin) AS begin + 1;\n'
            'SELECT m3(41) AS w;\n'
            '.print CANARY\n',
        )
        result.check_stdout("1")
        result.check_stdout("2")
        result.check_stdout("42")
        result.check_stdout("CANARY")

    def test_native_trigger_with_begin_identifiers(self, shell):
        # native triggers have no BEGIN ... END body - identifiers named begin stay identifiers
        result = run_lines(
            shell,
            'CREATE TABLE target(i INTEGER);\n'
            'CREATE TABLE audit(i INTEGER);\n'
            'CREATE TRIGGER begin AFTER INSERT ON target FOR EACH STATEMENT\n'
            '  INSERT INTO audit VALUES (1);\n'
            '.print CANARY\n'
            'INSERT INTO target VALUES (42);\n'
            'SELECT count(*) = 1 AS fired FROM audit;\n',
        )
        assert "CANARY" in result.stdout
        result.check_stdout("fired")
        result.check_stdout("true")

    def test_multibyte_identifier_with_keyword_suffix(self, shell):
        # a multi-byte identifier ending in a keyword is not classified as that keyword
        result = run_lines(
            shell,
            'SELECT 1 AS ébegin;\n'
            '.print CANARY\n',
        )
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
