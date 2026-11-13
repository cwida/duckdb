#include "catch.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/literal.hpp"
#include "duckdb/parser/parser.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Value expression conversion preserves VARIANT payloads and floating zero", "[literal][value_sql]") {
	DuckDB db;
	Connection con(db);
	for (auto sql :
	     {"'hello'::VARIANT", "7::SMALLINT::VARIANT", "{'x': 7::SMALLINT::VARIANT}::VARIANT",
	      "[7::SMALLINT::VARIANT, 'x'::VARIANT]::VARIANT", "'-0.0'::FLOAT", "'-0.0'::DOUBLE", "'-0.0'::FLOAT::VARIANT",
	      "'-0.0'::DOUBLE::VARIANT", "{'a': [{'b': 1::SMALLINT, 'c': 2::TINYINT, 'a': 3}], 'b': 4, 'c': 5}::VARIANT",
	      "{'a': [7::SMALLINT::VARIANT, {'b': 2::TINYINT, 'a': 3}::VARIANT]}::VARIANT"}) {
		INFO(sql);
		auto original = con.Query("SELECT " + string(sql));
		REQUIRE_NO_FAIL(*original);
		auto value = original->Collection().GetValue(0, 0);
		auto parsed = ConstantExpression::FromValue(value);
		auto rendered = parsed->ToString();
		INFO(rendered);
		auto restored = con.Query("SELECT " + rendered);
		REQUIRE_NO_FAIL(*restored);
		REQUIRE(restored->GetTypes() == original->GetTypes());
		REQUIRE_FALSE(ValueOperations::DistinctFrom(restored->Collection().GetValue(0, 0), value));
		if (value.type().id() == LogicalTypeId::VARIANT) {
			auto types = con.Query("SELECT variant_typeof(" + string(sql) + ") = variant_typeof(" + rendered + ")");
			REQUIRE_NO_FAIL(*types);
			REQUIRE(types->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
			auto strings = con.Query("SELECT variant_extract_string(" + string(sql) +
			                         ", '') = variant_extract_string(" + rendered + ", '')");
			REQUIRE_NO_FAIL(*strings);
			REQUIRE(strings->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
		}
		if (string(sql).find("-0.0") != string::npos) {
			auto signs = con.Query("SELECT 1.0 / (" + string(sql) + ")::DOUBLE = 1.0 / (" + rendered + ")::DOUBLE");
			REQUIRE_NO_FAIL(*signs);
			REQUIRE(signs->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
		}
	}
}

// Parses "SELECT <text>" and returns the constant the parser produced for it.
static Value ParseConstant(const string &text) {
	auto expressions = Parser::GetBuiltinParser().ParseExpressionList(text);
	REQUIRE(expressions.size() == 1);
	REQUIRE(expressions[0]->GetExpressionClass() == ExpressionClass::CONSTANT);
	return expressions[0]->Cast<ConstantExpression>().GetLiteral().ToValue();
}

static void RequireSameLiteralValue(const Value &actual, const Value &expected) {
	INFO("actual=" << actual.ToSQLString() << " (" << actual.type().ToString()
	               << ") expected=" << expected.ToSQLString() << " (" << expected.type().ToString() << ")");
	REQUIRE(actual.type() == expected.type());
	REQUIRE(!ValueOperations::DistinctFrom(actual, expected));
}

TEST_CASE("Literal::Number matches the parser's number typing", "[literal]") {
	duckdb::vector<string> numbers = {"0",
	                                  "1",
	                                  "-1",
	                                  "2147483647",
	                                  "2147483648",
	                                  "-2147483648",
	                                  "-2147483649",
	                                  "9223372036854775807",
	                                  "9223372036854775808",
	                                  "-9223372036854775808",
	                                  "-9223372036854775809",
	                                  "170141183460469231731687303715884105727",
	                                  "170141183460469231731687303715884105728",
	                                  "-170141183460469231731687303715884105728",
	                                  "340282366920938463463374607431768211455",
	                                  "340282366920938463463374607431768211456",
	                                  "123456789012345678901234567890123456789012345678901234567890",
	                                  "1_000",
	                                  "1_000_000",
	                                  "1.5",
	                                  "-1.5",
	                                  "0.5",
	                                  ".5",
	                                  "1.",
	                                  "1.50",
	                                  "1_000.5",
	                                  "1.000_5",
	                                  "123456789012345678.123456789012345678",
	                                  "1234567890123456789012345678901234567.89",
	                                  "12345678901234567890123456789012345678.9",
	                                  "123456789012345678901234567890123456789.0",
	                                  "1e3",
	                                  "1E3",
	                                  "1.5e3",
	                                  "1e-3",
	                                  "-1e3"};
	for (auto &text : numbers) {
		auto literal = Literal::Number(text);
		INFO("text=" << text);
		RequireSameLiteralValue(literal.ToValue(), ParseConstant(text));
		REQUIRE(literal.ToString() == text);
		RequireSameLiteralValue(Literal::Number(literal.ToString()).ToValue(), literal.ToValue());
	}
}

TEST_CASE("Literal kinds", "[literal]") {
	REQUIRE(Literal::Number("42").kind == LiteralKind::INTEGER);
	REQUIRE(Literal::Number("-42").kind == LiteralKind::INTEGER);
	REQUIRE(Literal::Number("4_2").kind == LiteralKind::INTEGER);
	REQUIRE(Literal::Number("4.2").kind == LiteralKind::NUMERIC);
	REQUIRE(Literal::Number("4e2").kind == LiteralKind::NUMERIC);
	REQUIRE(Literal::Number("4E2").kind == LiteralKind::NUMERIC);

	int64_t value;
	REQUIRE(Literal::Number("42").TryGetInt64(value));
	REQUIRE(value == 42);
	REQUIRE(Literal::Number("-9223372036854775808").TryGetInt64(value));
	REQUIRE(value == NumericLimits<int64_t>::Minimum());
	REQUIRE(!Literal::Number("9223372036854775808").TryGetInt64(value));
	REQUIRE(!Literal::Number("4.2").TryGetInt64(value));
	REQUIRE(!Literal::String("42").TryGetInt64(value));

	REQUIRE(Literal::Number("42").Negate().text == "-42");
	REQUIRE(Literal::Number("-42").Negate().text == "42");
	REQUIRE(Literal::Number("4.2").Negate().text == "-4.2");
	REQUIRE(Literal::Number("4.2").Negate().kind == LiteralKind::NUMERIC);

	REQUIRE(Literal::Null().IsNull());
	REQUIRE(Literal::Null().ToValue().IsNull());
	REQUIRE(Literal::Null().ToString() == "NULL");
	RequireSameLiteralValue(Literal::Boolean(true).ToValue(), Value::BOOLEAN(true));
	RequireSameLiteralValue(Literal::Boolean(false).ToValue(), Value::BOOLEAN(false));
	REQUIRE(Literal::Boolean(true).ToString() == "true");
	REQUIRE(Literal::Boolean(false).ToString() == "false");
	RequireSameLiteralValue(Literal::Integer(-7).ToValue(), Value::INTEGER(-7));
}

TEST_CASE("Literal strings, hex strings and bit strings", "[literal]") {
	RequireSameLiteralValue(Literal::String("it's").ToValue(), Value("it's"));
	REQUIRE(Literal::String("it's").ToString() == "'it''s'");
	RequireSameLiteralValue(Literal::String("").ToValue(), Value(""));
	REQUIRE(Literal::String("").ToString() == "''");

	RequireSameLiteralValue(Literal::Hex("FF00ab").ToValue(), ParseConstant("X'FF00ab'"));
	RequireSameLiteralValue(Literal::Hex("").ToValue(), ParseConstant("X''"));
	REQUIRE(Literal::Hex("FF").ToString() == "X'FF'");
	REQUIRE_THROWS_AS(Literal::Hex("F"), ParserException);
	REQUIRE_THROWS_AS(Literal::Hex("GG"), ParserException);

	RequireSameLiteralValue(Literal::Bit("0101").ToValue(), ParseConstant("B'0101'"));
	REQUIRE(Literal::Bit("0101").ToValue().type() == LogicalType::BIT);
	REQUIRE(Literal::Bit("0101").ToValue().ToString() == "0101");
	REQUIRE(Literal::Bit("0101").ToString() == "B'0101'");
	REQUIRE_THROWS_AS(Literal::Bit("012"), ParserException);
}

TEST_CASE("Literal equality and hashing", "[literal]") {
	REQUIRE(Literal::Number("1") == Literal::Number("1"));
	REQUIRE(Literal::Number("1").Hash() == Literal::Number("1").Hash());
	REQUIRE(Literal::Number("1") != Literal::Number("01"));
	REQUIRE(Literal::Number("1") != Literal::String("1"));
	REQUIRE(Literal::Number("1").Hash() != Literal::String("1").Hash());
	REQUIRE(Literal::Null() == Literal::Null());
	REQUIRE(Literal() != Literal::Null());
}
