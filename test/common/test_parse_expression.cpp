#include "catch.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/common/query_location.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include <type_traits>

using namespace duckdb;
using namespace std;

TEST_CASE("QueryLocation type semantics", "[parse_expression]") {
	// invalid sentinel (used for synthesized expressions with no source location)
	REQUIRE(!QueryLocation().IsValid());
	REQUIRE(!QueryLocation::Invalid().IsValid());

	QueryLocation location(5, 3);
	REQUIRE(location.IsValid());
	REQUIRE(location.Start() == 5);
	REQUIRE(location.End() == 8);

	// trivial conversion both ways with optional_idx
	optional_idx as_idx = location;
	REQUIRE(as_idx.GetIndex() == 5);
	QueryLocation from_idx = optional_idx(9);
	REQUIRE(from_idx.offset == 9);
	REQUIRE(from_idx.length == 0);
	REQUIRE(!QueryLocation(optional_idx()).IsValid());

	// merge produces the smallest enclosing location and ignores invalid locations
	auto merged = QueryLocation(2, 2).Merge(QueryLocation(6, 1)); // [2,4) U [6,7) -> [2,7)
	REQUIRE(merged.offset == 2);
	REQUIRE(merged.length == 5);
	REQUIRE(QueryLocation(2, 2).Merge(QueryLocation::Invalid()) == QueryLocation(2, 2));
	REQUIRE(QueryLocation::Invalid().Merge(QueryLocation(2, 2)) == QueryLocation(2, 2));
}

TEST_CASE("Parsed expressions carry source locations", "[parse_expression]") {
	// Note: ParseExpressionList wraps the input in a SELECT, so offsets are relative to that wrapper.
	// The location length is what matters here and is independent of the wrapper.

	// a numeric constant spans its own token
	auto result = Parser::GetBuiltinParser().ParseExpressionList("42");
	REQUIRE(result.size() == 1);
	auto location = result[0]->GetQueryLocation();
	REQUIRE(location.IsValid());
	REQUIRE(location.length == 2);

	// a column reference encloses the full identifier
	result = Parser::GetBuiltinParser().ParseExpressionList("abc");
	location = result[0]->GetQueryLocation();
	REQUIRE(location.IsValid());
	REQUIRE(location.length == 3);

	// a qualified column reference encloses the whole qualified name (tbl.abc)
	result = Parser::GetBuiltinParser().ParseExpressionList("tbl.abc");
	location = result[0]->GetQueryLocation();
	REQUIRE(location.IsValid());
	REQUIRE(location.length == 7);
}

TEST_CASE("Parse Expression List valid expressions", "[parse_expression]") {
	auto result = Parser::GetBuiltinParser().ParseExpressionList("x");
	REQUIRE(result.size() == 1);

	result = Parser::GetBuiltinParser().ParseExpressionList("x, y, z");
	REQUIRE(result.size() == 3);

	result = Parser::GetBuiltinParser().ParseExpressionList("FIRST(x) AS x");
	REQUIRE(result.size() == 1);

	result = Parser::GetBuiltinParser().ParseExpressionList("x + 1, y * 2");
	REQUIRE(result.size() == 2);
}

TEST_CASE("Parse Expression List rejects invalid clauses", "[parse_expression]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return;
#endif
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("FIRST(x) AS x WHERE x = 'bad'"), ParserException);
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("first(x) AS x HAVING x"), ParserException);
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("first(x) AS x QUALIFY x"), ParserException);
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("first(x) AS x USING SAMPLE 1 ROWS"),
	                  ParserException);
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("first(x) AS x GROUP BY x"), ParserException);
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("first(x) AS x ORDER BY x"), ParserException);
	REQUIRE_THROWS_AS(Parser::GetBuiltinParser().ParseExpressionList("x LIMIT 1"), ParserException);
}

static_assert(!std::is_default_constructible<Parser>::value, "Parser requires an explicit environment");
static_assert(!std::is_default_constructible<ParserOptions>::value, "Standalone options must be explicit");

TEST_CASE("Parser construction selects settings explicitly", "[parse_expression]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET integer_division = true"));
	REQUIRE_NO_FAIL(con.Query("SET preserve_identifier_case = 'lowercase'"));
	REQUIRE_NO_FAIL(con.Query("SET regex_match_operator_semantics = 'full'"));

	Parser parser(*con.context);
	auto expressions = parser.ParseExpressionList("MixedCase, 3 / 2, 'abc' ~ 'b'");
	REQUIRE(expressions[0]->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName() == "mixedcase");
	REQUIRE(expressions[1]->Cast<FunctionExpression>().GetQualifiedName().Name() == "//");
	REQUIRE(expressions[2]->Cast<FunctionExpression>().GetQualifiedName().Name() == "regexp_full_match");

	auto builtin = Parser::GetBuiltinParser();
	expressions = builtin.ParseExpressionList("MixedCase, 3 / 2, 'abc' ~ 'b'");
	REQUIRE(expressions[0]->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName() == "MixedCase");
	REQUIRE(expressions[1]->Cast<FunctionExpression>().GetQualifiedName().Name() == "/");
	REQUIRE(expressions[2]->Cast<FunctionExpression>().GetQualifiedName().Name() == "regexp_matches");

	REQUIRE_NO_FAIL(con.Query("SET integer_division = false"));
	expressions = parser.ParseExpressionList("3 / 2");
	REQUIRE(expressions[0]->Cast<FunctionExpression>().GetQualifiedName().Name() == "//");
	expressions = Parser(*con.context).ParseExpressionList("3 / 2");
	REQUIRE(expressions[0]->Cast<FunctionExpression>().GetQualifiedName().Name() == "/");

	expressions = Parser(*con.context, IdentifierCaseMode::PRESERVE_CASE).ParseExpressionList("MixedCase");
	REQUIRE(expressions[0]->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName() == "MixedCase");
}

TEST_CASE("Standalone parser options can be customized", "[parse_expression]") {
	auto options = ParserOptions::Builtin();
	options.integer_division = true;
	options.identifier_case_mode = IdentifierCaseMode::UPPERCASE;
	Parser parser(options);
	auto expressions = parser.ParseExpressionList("MixedCase, 3 / 2");
	REQUIRE(expressions[0]->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName() == "MIXEDCASE");
	REQUIRE(expressions[1]->Cast<FunctionExpression>().GetQualifiedName().Name() == "//");
}
