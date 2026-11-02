# fmt: off

import os

import pytest

from conftest import ShellTest

# tests for statement completeness (ShellState::SQLIsComplete)
# the shell submits the input buffer once the parser accepts it as a complete,
# semicolon-terminated statement sequence - framing follows the active grammar


def run_lines(shell, text):
    return ShellTest(shell).run_raw(text)


def grammar_demo_path(shell):
    # the loadable grammar demo is built next to the shell binary in the build tree
    build_dir = os.path.dirname(os.path.abspath(shell))
    return os.path.join(build_dir, 'test', 'extension', 'loadable_grammar_extension_demo.duckdb_extension')


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

    def test_malformed_numeric_literal_is_submitted(self, shell):
        # a numeric literal the matcher rejects outright does not break framing
        result = run_lines(
            shell,
            'SELECT 1e2e3;\n'
            '.print CANARY\n',
        )
        assert "Error" in result.stderr
        assert "CANARY" in result.stdout

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


class TestExtendedGrammarCompleteness(object):
    # with a grammar extension that adds a block statement containing semicolon-separated
    # statements, the shell frames the whole block as one statement

    def block_shell(self, shell):
        extension = grammar_demo_path(shell)
        if not os.path.exists(extension):
            pytest.skip("loadable_grammar_extension_demo is not built")
        return (
            f"LOAD '{extension}';\n"
            "SET active_grammar_extensions = ['block_statement_syntax'];\n"
        )

    def test_block_single_submission(self, shell):
        prelude = self.block_shell(shell)
        result = ShellTest(shell, ["-unsigned"]).run_raw(
            prelude +
            'BEGIN_DEMO_BLOCK\n'
            '  SELECT 1;\n'
            '  SELECT 2;\n'
            'END_DEMO_BLOCK;\n'
            '.print CANARY\n',
        )
        assert "Parser Error" not in result.stderr
        assert "cannot commit" not in result.stderr
        assert "demo block" in result.stdout
        assert "CANARY" in result.stdout

    def test_block_nested_statements(self, shell):
        # strings, comments and CASE expressions inside the block do not end it early
        prelude = self.block_shell(shell)
        result = ShellTest(shell, ["-unsigned"]).run_raw(
            prelude +
            'BEGIN_DEMO_BLOCK\n'
            "  SELECT 'END_DEMO_BLOCK; not a real end';\n"
            '  -- comment with END_DEMO_BLOCK; inside\n'
            '  SELECT CASE WHEN 1 > 0 THEN 1 ELSE 0 END;\n'
            'END_DEMO_BLOCK;\n'
            '.print CANARY\n',
        )
        assert "Parser Error" not in result.stderr
        assert "demo block" in result.stdout
        assert "CANARY" in result.stdout

    def test_parser_extension_claim_before_block(self, shell):
        # a leading statement only a parse_function extension accepts does not end the
        # completeness check early - the open block after it still asks for more input
        extension = grammar_demo_path(shell)
        quack = os.path.join(os.path.dirname(extension), 'loadable_extension_demo.duckdb_extension')
        if not os.path.exists(extension) or not os.path.exists(quack):
            pytest.skip("demo extensions are not built")
        result = ShellTest(shell, ["-unsigned"]).run_raw(
            f"LOAD '{extension}';\n"
            f"LOAD '{quack}';\n"
            "SET active_grammar_extensions = ['block_statement_syntax'];\n"
            'quack quack quack; BEGIN_DEMO_BLOCK\n'
            '  SELECT 1;\n'
            'END_DEMO_BLOCK;\n'
            '.print CANARY\n',
        )
        assert "Parser Error" not in result.stderr
        assert "QUACK" in result.stdout
        assert "demo block" in result.stdout
        assert "CANARY" in result.stdout

    def test_fatal_token_inside_block(self, shell):
        # a zero-length delimited identifier cannot be repaired by more input - the buffer is
        # submitted and the error surfaces instead of an endless continuation prompt
        prelude = self.block_shell(shell)
        result = ShellTest(shell, ["-unsigned"]).run_raw(
            prelude +
            'BEGIN_DEMO_BLOCK SELECT "";\n'
            '.print CANARY\n',
        )
        assert "zero-length delimited identifier" in result.stderr
        assert "CANARY" in result.stdout

    def test_block_without_activation_splits(self, shell):
        # the same input splits at top-level semicolons when the extension grammar is not active
        extension = grammar_demo_path(shell)
        if not os.path.exists(extension):
            pytest.skip("loadable_grammar_extension_demo is not built")
        result = ShellTest(shell, ["-unsigned"]).run_raw(
            f"LOAD '{extension}';\n"
            'BEGIN_DEMO_BLOCK\n'
            '  SELECT 1;\n'
            '  SELECT 2;\n'
            'END_DEMO_BLOCK;\n'
            '.print CANARY\n',
        )
        assert result.stderr.count("Error") >= 2
        assert "demo block" not in result.stdout
        assert "CANARY" in result.stdout
