#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static unique_ptr<LogicalOperator> BindNestedCreateIndex(ClientContext &, TableFunctionBindInput &input,
                                                         TableIndex bind_index, vector<Identifier> &return_names) {
	Parser parser;
	parser.ParseQuery("CREATE INDEX nested_idx ON nested_index_target (value)");
	auto bound = input.binder->Bind(*parser.statements[0]);
	bound.plan->ResolveOperatorTypes();

	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(make_uniq<BoundColumnRefExpression>(bound.types[0], bound.plan->GetColumnBindings()[0]));
	return_names.emplace_back("Count");
	auto result = make_uniq<LogicalProjection>(bind_index, std::move(expressions));
	result->children.push_back(std::move(bound.plan));
	return std::move(result);
}

TEST_CASE("Nested CREATE INDEX plans preserve bindings and required input columns", "[api][index]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE nested_index_target(id INTEGER, value VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO nested_index_target VALUES (1, 'one'), (2, 'two')"));

	TableFunction function("create_nested_index", {}, nullptr, nullptr);
	function.bind_operator = BindNestedCreateIndex;
	CreateTableFunctionInfo function_info(function);
	con.BeginTransaction();
	auto &catalog = Catalog::GetSystemCatalog(*con.context);
	catalog.CreateTableFunction(*con.context, function_info);
	con.Commit();

	// The constant projection leaves the nested result unused while the index still requires its columns and row IDs.
	REQUIRE_NO_FAIL(con.Query("SELECT 42 FROM create_nested_index()"));
	auto result = con.Query("SELECT count(*) FROM duckdb_indexes() WHERE index_name = 'nested_idx'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}
