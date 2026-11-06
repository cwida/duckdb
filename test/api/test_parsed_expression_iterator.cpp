#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"

using namespace duckdb;

static void CollectParameters(QueryNode &node, identifier_set_t &found);

static void CollectParameters(ParsedExpression &expr, identifier_set_t &found) {
	if (expr.GetExpressionClass() == ExpressionClass::PARAMETER) {
		found.insert(expr.Cast<ParameterExpression>().Identifier());
	}
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		CollectParameters(*expr.Cast<SubqueryExpression>().Subquery()->node, found);
	}
	ParsedExpressionIterator::EnumerateChildren(expr,
	                                            [&](ParsedExpression &child) { CollectParameters(child, found); });
}

static void CollectParameters(QueryNode &node, identifier_set_t &found) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { CollectParameters(*child, found); });
}

static unique_ptr<SelectStatement> ParseSelect(Connection &con, const string &sql) {
	Parser parser(*con.context);
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::SELECT_STATEMENT);
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

static identifier_set_t NumberedParameters(const SelectStatement &statement) {
	identifier_set_t result;
	for (auto &entry : statement.named_param_map) {
		result.insert(entry.first);
	}
	return result;
}

// The transformer numbers every parameter it parses, so a complete walk finds exactly the numbered ones.
static void RequireWalkFindsEveryParameter(Connection &con, const string &sql, idx_t expected_count) {
	INFO(sql);
	auto statement = ParseSelect(con, sql);
	auto numbered = NumberedParameters(*statement);
	REQUIRE(numbered.size() == expected_count);

	identifier_set_t found;
	CollectParameters(*statement->node, found);
	CHECK(found == numbered);

	auto reparsed = ParseSelect(con, statement->ToString());
	identifier_set_t found_reparsed;
	CollectParameters(*reparsed->node, found_reparsed);
	CHECK(found_reparsed == numbered);
}

TEST_CASE("ParsedExpressionIterator visits every expression of a parsed query", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	// reference positions: a join condition and MATCH_RECOGNIZE MEASURES
	RequireWalkFindsEveryParameter(con, "SELECT * FROM (VALUES (1)) a(x) JOIN (VALUES (2)) b(y) ON a.x + $1 = b.y", 1);
	RequireWalkFindsEveryParameter(con,
	                               "SELECT m FROM (SELECT 1 AS x) t MATCH_RECOGNIZE (MEASURES FIRST(x) + $1 AS m "
	                               "ONE ROW PER MATCH PATTERN (A) DEFINE A AS true)",
	                               1);

	RequireWalkFindsEveryParameter(con,
	                               "SELECT t.v FROM (VALUES (0)) u(z) JOIN (VALUES (4), (10)) t(v) "
	                               "NEAREST 1 BY DISTANCE abs(t.v - $1)",
	                               1);
	RequireWalkFindsEveryParameter(con, "SELECT * FROM (PIVOT (SELECT 7 AS v, 1 AS a) ON (a + $1) IN (2) USING max(v))",
	                               1);
	RequireWalkFindsEveryParameter(con, "SELECT * FROM (PIVOT (SELECT 7 AS v, 1 AS a) ON a IN ($1 + 1) USING max(v))",
	                               1);
	RequireWalkFindsEveryParameter(con,
	                               "SELECT m FROM (SELECT $1 AS x) t MATCH_RECOGNIZE (MEASURES FIRST(x) AS m "
	                               "ONE ROW PER MATCH PATTERN (A) DEFINE A AS true)",
	                               1);
	RequireWalkFindsEveryParameter(con, "SELECT * FROM t AT (VERSION => $1)", 1);

	// a PIVOT and a NEAREST join inside a MATCH_RECOGNIZE input, and a MATCH_RECOGNIZE inside a scalar subquery
	RequireWalkFindsEveryParameter(con,
	                               "SELECT m FROM (PIVOT (SELECT 7 AS v, 1 AS a) ON (a + $1) IN (2) USING max(v)) p "
	                               "MATCH_RECOGNIZE (MEASURES FIRST(\"2\") AS m ONE ROW PER MATCH PATTERN (A) "
	                               "DEFINE A AS true)",
	                               1);
	RequireWalkFindsEveryParameter(con,
	                               "SELECT (SELECT m FROM ((VALUES (0)) u(z) JOIN (VALUES (4), (10)) t(v) "
	                               "NEAREST 1 BY DISTANCE abs(t.v - $1)) MATCH_RECOGNIZE (MEASURES FIRST(v) + $2 AS m "
	                               "ONE ROW PER MATCH PATTERN (A) DEFINE A AS v > $3))",
	                               3);
}

static void CollectColumnNames(ParsedExpression &expr, identifier_set_t &found) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		found.insert(expr.Cast<ColumnRefExpression>().GetColumnName());
	}
	ParsedExpressionIterator::EnumerateChildren(expr,
	                                            [&](ParsedExpression &child) { CollectColumnNames(child, found); });
}

static identifier_set_t WalkedColumnNames(SelectStatement &statement) {
	identifier_set_t found;
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    *statement.node, [&](unique_ptr<ParsedExpression> &child) { CollectColumnNames(*child, found); });
	return found;
}

TEST_CASE("ParsedExpressionIterator visits UNPIVOT entry expressions but not UNPIVOT column names", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	// a, c and d are names of source columns; b and e sit inside expressions the binder evaluates
	for (auto &sql :
	     {string("SELECT * FROM (UNPIVOT (SELECT 1 AS a, 2 AS b, 3 AS c, 4 AS d, 5 AS e) ON a, (c, d), "
	             "(b + $1) AS x, COLUMNS(lambda col: col = e) INTO NAME k VALUE v)"),
	      string("SELECT * FROM (SELECT 1 AS a, 2 AS b, 3 AS c, 4 AS d, 5 AS e) UNPIVOT (v FOR k IN (a, (c, "
	             "d), (b + $1) AS x, COLUMNS(lambda col: col = e), 2000))")}) {
		RequireWalkFindsEveryParameter(con, sql, 1);
		auto statement = ParseSelect(con, sql);
		CHECK(WalkedColumnNames(*statement) == identifier_set_t {Identifier("b"), Identifier("col"), Identifier("e")});
		auto reparsed = ParseSelect(con, statement->ToString());
		CHECK(WalkedColumnNames(*reparsed) == identifier_set_t {Identifier("b"), Identifier("col"), Identifier("e")});
	}
}

TEST_CASE("ParsedExpressionIterator reports the table references inside a MATCH_RECOGNIZE input", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto statement = ParseSelect(con, "SELECT * FROM (SELECT * FROM inner_table) t MATCH_RECOGNIZE (MEASURES "
	                                  "FIRST(x) AS m ONE ROW PER MATCH PATTERN (A) DEFINE A AS true)");
	vector<Identifier> base_tables;
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    *statement->node, [](unique_ptr<ParsedExpression> &) {},
	    [&](TableRef &ref) {
		    if (ref.type == TableReferenceType::BASE_TABLE) {
			    base_tables.push_back(ref.Cast<BaseTableRef>().Table());
		    }
	    });
	REQUIRE(base_tables.size() == 1);
	REQUIRE(base_tables[0] == Identifier("inner_table"));
}

static unique_ptr<ParameterExpression> MakeParameter(const string &identifier) {
	auto parameter = make_uniq<ParameterExpression>();
	parameter->IdentifierMutable() = Identifier(identifier);
	return parameter;
}

TEST_CASE("ParsedExpressionIterator visits the duplicate eliminated columns of a join", "[api][parser]") {
	// the relation API's delim join fills duplicate_eliminated_columns
	JoinRef join(JoinRefType::REGULAR);
	join.left = make_uniq<BaseTableRef>();
	join.right = make_uniq<BaseTableRef>();
	join.condition = MakeParameter("1");
	join.duplicate_eliminated_columns.push_back(MakeParameter("2"));
	identifier_set_t found_in_join;
	ParsedExpressionIterator::EnumerateTableRefChildren(
	    join, [&](unique_ptr<ParsedExpression> &child) { CollectParameters(*child, found_in_join); });
	REQUIRE(found_in_join == identifier_set_t {Identifier("1"), Identifier("2")});
}

static bool ParsedSelectsEqual(Connection &con, const string &left, const string &right) {
	INFO(left);
	INFO(right);
	auto left_statement = ParseSelect(con, left);
	auto right_statement = ParseSelect(con, right);
	REQUIRE(left_statement->Equals(*left_statement->Copy()));
	return left_statement->Equals(*right_statement);
}

TEST_CASE("Table references that differ only in an iterated expression are not equal", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);

	CHECK(!ParsedSelectsEqual(con, "SELECT * FROM (PIVOT (SELECT 2 AS c, 7 AS v) ON c IN (1 + 1) USING max(v))",
	                          "SELECT * FROM (PIVOT (SELECT 2 AS c, 7 AS v) ON c IN (1 + 2) USING max(v))"));
	CHECK(!ParsedSelectsEqual(con, "SELECT * FROM (UNPIVOT (SELECT 1 AS a) ON (a + 1) AS x INTO NAME n VALUE v)",
	                          "SELECT * FROM (UNPIVOT (SELECT 1 AS a) ON (a + 2) AS x INTO NAME n VALUE v)"));
	CHECK(ParsedSelectsEqual(con, "SELECT * FROM (PIVOT (SELECT 2 AS c, 7 AS v) ON c IN (1 + 1) USING max(v))",
	                         "SELECT * FROM (PIVOT (SELECT 2 AS c, 7 AS v) ON c IN (1 + 1) USING max(v))"));

	JoinRef join(JoinRefType::REGULAR);
	join.left = make_uniq<BaseTableRef>();
	join.right = make_uniq<BaseTableRef>();
	join.duplicate_eliminated_columns.push_back(MakeParameter("1"));
	auto other = join.Copy();
	REQUIRE(join.Equals(*other));
	other->Cast<JoinRef>().duplicate_eliminated_columns[0] = MakeParameter("2");
	CHECK(!join.Equals(*other));
}
