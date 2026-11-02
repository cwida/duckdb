#include "catch.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

static Value EvalConstantExpression(Connection &con, const string &expr) {
	auto result = con.Query("SELECT " + expr);
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0);
}

// Binds the parsed expression directly (no SQL text in between) and evaluates it.
static Value BindAndEvaluate(Connection &con, duckdb::unique_ptr<ParsedExpression> expr) {
	con.BeginTransaction();
	auto &context = *con.context;
	auto binder = Binder::CreateBinder(context);
	ConstantBinder constant_binder(*binder, context, "test");
	auto bound = constant_binder.Bind(expr);
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound, true);
	con.Rollback();
	return value;
}

static void RequireSameExpressionValue(const Value &actual, const Value &expected) {
	INFO("actual=" << actual.ToSQLString() << " (" << actual.type().ToString()
	               << ") expected=" << expected.ToSQLString() << " (" << expected.type().ToString() << ")");
	REQUIRE(actual.type() == expected.type());
	REQUIRE(!ValueOperations::DistinctFrom(actual, expected));
	REQUIRE(actual.type().EqualsIncludingCollation(expected.type()));
}

// Asserts the round-trip contract of ConstantExpression::FromValue: the expression binds back to the same
// value, and so does its SQL text.
static void RequireExpressionRoundTrip(Connection &con, const string &expr) {
	auto original = EvalConstantExpression(con, expr);
	auto parsed = ConstantExpression::FromValue(original);
	auto sql = parsed->ToString();
	INFO("expr=" << expr << " FromValue=" << sql);
	RequireSameExpressionValue(BindAndEvaluate(con, parsed->Copy()), original);
	RequireSameExpressionValue(EvalConstantExpression(con, sql), original);
}

static string Render(const Value &value) {
	return ConstantExpression::FromValue(value)->ToString();
}

TEST_CASE("ConstantExpression::FromValue emits bare literals only when they re-bind exactly", "[api]") {
	REQUIRE(Render(Value::INTEGER(5)) == "5");
	REQUIRE(Render(Value::INTEGER(-5)) == "-5");
	REQUIRE(Render(Value::BIGINT(5)) == "CAST(5 AS BIGINT)");
	REQUIRE(Render(Value::BIGINT(NumericLimits<int64_t>::Minimum())) == "-9223372036854775808");
	REQUIRE(Render(Value::TINYINT(5)) == "CAST(5 AS TINYINT)");
	REQUIRE(Render(Value::UBIGINT(5)) == "CAST(5 AS UBIGINT)");
	REQUIRE(Render(Value::BOOLEAN(true)) == "true");
	REQUIRE(Render(Value("it's")) == "'it''s'");
	REQUIRE(Render(Value()) == "NULL");
	REQUIRE(Render(Value(LogicalType::DATE)) == "CAST(NULL AS DATE)");
	REQUIRE(Render(Value::DOUBLE(1.5)) == "CAST(1.5 AS DOUBLE)");
	REQUIRE(Render(Value::DATE(date_t::epoch())) == "CAST('1970-01-01' AS DATE)");
	REQUIRE(Render(Value::BLOB_RAW(string("\x00\xff", 2))) == "X'00FF'");
	REQUIRE(Render(Value::BIT("0101")) == "B'0101'");
	REQUIRE(Render(Value::LIST(LogicalType::INTEGER, {})) == "CAST(\"system\".main.list_value() AS INTEGER[])");
	REQUIRE(Render(Value::LIST(LogicalType::INTEGER, {Value::INTEGER(1), Value::INTEGER(2)})) ==
	        "\"system\".main.list_value(1, 2)");
	child_list_t<Value> fields;
	fields.emplace_back("a", Value::INTEGER(1));
	fields.emplace_back("b", Value("x"));
	REQUIRE(Render(Value::STRUCT(std::move(fields))) == "\"system\".main.struct_pack(a := 1, b := 'x')");
}

TEST_CASE("ConstantExpression::FromValue round-trips scalar values", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	RequireExpressionRoundTrip(con, "NULL");
	RequireExpressionRoundTrip(con, "NULL::INTEGER");
	RequireExpressionRoundTrip(con, "TRUE");
	RequireExpressionRoundTrip(con, "42");
	RequireExpressionRoundTrip(con, "-42");
	RequireExpressionRoundTrip(con, "42::TINYINT");
	RequireExpressionRoundTrip(con, "42::SMALLINT");
	RequireExpressionRoundTrip(con, "42::BIGINT");
	RequireExpressionRoundTrip(con, "42::UTINYINT");
	RequireExpressionRoundTrip(con, "42::UINTEGER");
	RequireExpressionRoundTrip(con, "42::UBIGINT");
	RequireExpressionRoundTrip(con, "42::HUGEINT");
	RequireExpressionRoundTrip(con, "42::UHUGEINT");
	RequireExpressionRoundTrip(con, "9223372036854775808");
	RequireExpressionRoundTrip(con, "-170141183460469231731687303715884105728");
	RequireExpressionRoundTrip(con, "123456789012345678901234567890123456789012345678901234567890");
	RequireExpressionRoundTrip(con, "1.50");
	RequireExpressionRoundTrip(con, "1.50::DECIMAL(18,2)");
	RequireExpressionRoundTrip(con, "-1.5::DECIMAL(4,1)");
	RequireExpressionRoundTrip(con, "1.5::FLOAT");
	RequireExpressionRoundTrip(con, "1.5::DOUBLE");
	RequireExpressionRoundTrip(con, "1e100");
	RequireExpressionRoundTrip(con, "'inf'::DOUBLE");
	RequireExpressionRoundTrip(con, "'-inf'::FLOAT");
	RequireExpressionRoundTrip(con, "'nan'::DOUBLE");
	RequireExpressionRoundTrip(con, "'hello'");
	RequireExpressionRoundTrip(con, "''");
	RequireExpressionRoundTrip(con, "'it''s'");
	RequireExpressionRoundTrip(con, "'a\nb'");
	RequireExpressionRoundTrip(con, "'\\xAA\\x00'::BLOB");
	RequireExpressionRoundTrip(con, "''::BLOB");
	RequireExpressionRoundTrip(con, "'0101'::BIT");
	RequireExpressionRoundTrip(con, "B'110010'");
	RequireExpressionRoundTrip(con, "''::BIT");
	RequireExpressionRoundTrip(con, "DATE '2020-01-02'");
	RequireExpressionRoundTrip(con, "TIME '12:34:56.789'");
	RequireExpressionRoundTrip(con, "TIMESTAMP '2020-01-02 12:34:56.789'");
	RequireExpressionRoundTrip(con, "TIMESTAMP_NS '2020-01-02 12:34:56.789123456'");
	RequireExpressionRoundTrip(con, "INTERVAL '1 day 2 hours'");
	RequireExpressionRoundTrip(con,
	                           "to_months(-2147483648)+to_days(-2147483648)+to_microseconds(-9223372036854775808)");
	RequireExpressionRoundTrip(con, "'POINT ZM(1.25 -2.5 3.75 4.5)'::GEOMETRY('OGC:CRS84')");
	RequireExpressionRoundTrip(con, "NULL::GEOMETRY('OGC:CRS84')");
	RequireExpressionRoundTrip(con, "'5ecb6a72-1fc3-4b5f-9d8a-0d3a4b5c6d7e'::UUID");
	RequireExpressionRoundTrip(con, "'a'::ENUM('a', 'b')");
	// the JSON alias only exists when the json extension is part of the build
	if (!con.Query("SELECT '{}'::JSON")->HasError()) {
		RequireExpressionRoundTrip(con, "'{\"a\": 1}'::JSON");
	}
}

TEST_CASE("ConstantExpression::FromValue round-trips pointer values", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	for (auto pointer : {uintptr_t(0), uintptr_t(42), uintptr_t(1) << (sizeof(uintptr_t) * 4), ~uintptr_t(0)}) {
		auto original = Value::POINTER(pointer);
		auto parsed = ConstantExpression::FromValue(original);
		REQUIRE(parsed->Cast<ConstantExpression>().GetLiteral().IsPointer());
		INFO("pointer=" << pointer << " FromValue=" << parsed->ToString());
		RequireSameExpressionValue(BindAndEvaluate(con, parsed->Copy()), original);
		REQUIRE(parsed->ToString() == original.ToString());
	}
}

TEST_CASE("ConstantExpression::FromValue round-trips nested values", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	RequireExpressionRoundTrip(con, "[1, 2, 3]");
	RequireExpressionRoundTrip(con, "[]::INTEGER[]");
	RequireExpressionRoundTrip(con, "[NULL]::INTEGER[]");
	RequireExpressionRoundTrip(con, "['a', NULL]");
	RequireExpressionRoundTrip(con, "[[1], [], [2, 3]]");
	RequireExpressionRoundTrip(con, "[1, 2]::BIGINT[]");
	RequireExpressionRoundTrip(con, "[1, 2, 3]::INTEGER[3]");
	RequireExpressionRoundTrip(con, "[[1, 2], [3, 4]]::INTEGER[2][2]");
	RequireExpressionRoundTrip(con, "{'a': 1, 'b': 'x'}");
	RequireExpressionRoundTrip(con, "{'a': NULL}::STRUCT(a INTEGER)");
	RequireExpressionRoundTrip(con, "{'it''s': 1}");
	RequireExpressionRoundTrip(con, "{'a': {'b': [1, 2]}}");
	RequireExpressionRoundTrip(con, "row(1, 'x')");
	RequireExpressionRoundTrip(con, "(1,)");
	RequireExpressionRoundTrip(con, "MAP {'a': 1, 'b': 2}");
	RequireExpressionRoundTrip(con, "MAP {}::MAP(VARCHAR, INTEGER)");
	RequireExpressionRoundTrip(con, "MAP {1: [1, 2]}");
	RequireExpressionRoundTrip(con, "union_value(num := 42)::UNION(num INTEGER, str VARCHAR)");
	RequireExpressionRoundTrip(con, "union_value(str := 'x')::UNION(num INTEGER, str VARCHAR)");
	RequireExpressionRoundTrip(con, "NULL::STRUCT(a INTEGER)");
	RequireExpressionRoundTrip(con, "NULL::INTEGER[]");
	RequireExpressionRoundTrip(con, "NULL::MAP(VARCHAR, INTEGER)");
	RequireExpressionRoundTrip(con, "'A' COLLATE nocase");
	RequireExpressionRoundTrip(con, "['A' COLLATE nocase, NULL]");
	RequireExpressionRoundTrip(con, "CASE WHEN false THEN ['A' COLLATE nocase] ELSE [] END");
	RequireExpressionRoundTrip(con, "CASE WHEN false THEN {'s': 'A' COLLATE nocase} ELSE NULL END");
	RequireExpressionRoundTrip(con, "CASE WHEN false THEN map([1], ['A' COLLATE nocase]) ELSE NULL END");
	RequireExpressionRoundTrip(con, "get_type(['A' COLLATE nocase])");
	RequireExpressionRoundTrip(con, "[get_type(42), NULL]");
	RequireExpressionRoundTrip(con, "(SELECT sum(i) EXPORT_STATE FROM range(3) t(i))");

	// Parent metadata need not be repeated in the stored child Values.
	auto collated = Value::ARRAY(LogicalType::VARCHAR, {Value("A")})
	                    .WithType(LogicalType::ARRAY(LogicalType::VARCHAR_COLLATION("nocase"), 1));
	auto expression = ConstantExpression::FromValue(collated);
	RequireSameExpressionValue(BindAndEvaluate(con, expression->Copy()), collated);
	RequireSameExpressionValue(EvalConstantExpression(con, expression->ToString()), collated);
}
