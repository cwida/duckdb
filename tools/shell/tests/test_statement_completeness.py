# fmt: off

from conftest import ShellTest

# tests for statement completeness (ShellState::SQLIsComplete)
# the shell submits the input buffer once the parser accepts it as a complete,
# semicolon-terminated statement sequence - framing follows the active grammar


def run_lines(shell, text):
    return ShellTest(shell).run_raw(text)


class TestDefaultGrammarCompleteness(object):
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
        # begin aliases and parameters inside valid macros do not change framing
        result = run_lines(
            shell,
            'CREATE FUNCTION m1() AS TABLE SELECT 1 AS begin;\n'
            'SELECT * FROM m1();\n'
            'CREATE FUNCTION m2() AS (SELECT 2 AS begin);\n'
            'SELECT m2() AS v;\n'
            'CREATE FUNCTION m3() AS 0, (begin) AS begin + 1;\n'
            'SELECT m3(41) AS w;\n'
            'CREATE FUNCTION m4(begin$arg) AS begin$arg + 1;\n'
            'SELECT m4(99) AS x;\n'
            '.print CANARY\n',
        )
        result.check_stdout("1")
        result.check_stdout("2")
        result.check_stdout("42")
        result.check_stdout("100")
        result.check_stdout("CANARY")

    def test_native_trigger_with_begin_identifiers(self, shell):
        # native triggers have a single DML body - identifiers named begin stay identifiers
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
            'SELECT $end$c; d$end$ AS t;\n'
            '.print CANARY\n',
        )
        result.check_stdout("a; b")
        result.check_stdout("c; d")
        result.check_stdout("CANARY")

    def test_multiline_dollar_quoted_string(self, shell):
        # an unterminated dollar-quoted string keeps accumulating input
        result = run_lines(
            shell,
            'SELECT $$a;\n'
            'b$$ AS s;\n'
            '.print CANARY\n',
        )
        result.check_stdout("a;")
        result.check_stdout("CANARY")
        assert "Parser Error" not in result.stderr

    def test_multiline_string_with_semicolon(self, shell):
        # an unterminated string literal keeps accumulating input
        result = run_lines(
            shell,
            "SELECT 'a;\n"
            "b' AS s;\n"
            '.print CANARY\n',
        )
        result.check_stdout("a;")
        result.check_stdout("CANARY")
        assert "Parser Error" not in result.stderr

    def test_multiline_block_comment(self, shell):
        # an unterminated block comment keeps accumulating input
        result = run_lines(
            shell,
            'SELECT 42 AS v; /* note\n'
            'more */;\n'
            '.print CANARY\n',
        )
        result.check_stdout("42")
        result.check_stdout("CANARY")
        assert "Parser Error" not in result.stderr

    def test_trailing_line_comment(self, shell):
        # a line comment after the final semicolon does not block completion
        result = run_lines(
            shell,
            'SELECT 42 AS v; -- done\n'
            '.print CANARY\n',
        )
        result.check_stdout("42")
        result.check_stdout("CANARY")

    def test_syntax_error_is_submitted(self, shell):
        # input the grammar rejects mid-way is submitted, so the error surfaces
        result = run_lines(
            shell,
            'SELECT FROM WHERE;\n'
            '.print CANARY\n'
            'SELECT 42 AS v;\n',
        )
        assert result.stderr.count("Parser Error") == 1
        assert "CANARY" in result.stdout
        assert "42" in result.stdout

    def test_unsupported_body_splits_per_grammar(self, shell):
        # without a grammar extension that adds procedural statement forms, BEGIN ... END routine
        # bodies are not part of the grammar - the buffer splits at top-level semicolons as before
        result = run_lines(
            shell,
            'CREATE OR REPLACE FUNCTION f(n INTEGER) RETURNS INTEGER AS BEGIN\n'
            '  SELECT 1;\n'
            '  RETURN n;\n'
            'END;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Parser Error") >= 2
        assert "CANARY" in result.stdout
