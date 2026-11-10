#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/planner.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static optional_ptr<LogicalOperator> FindCommonSubplanOperator(LogicalOperator &op, LogicalOperatorType type) {
	if (op.type == type) {
		return op;
	}
	for (auto &child : op.children) {
		auto result = FindCommonSubplanOperator(*child, type);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

static void ClearCommonSubplanProjectionMaps(LogicalOperator &op) {
	for (idx_t i = 0; i < op.children.size(); i++) {
		auto projection_map = LogicalOperatorVisitor::GetProjectionMap(op, i);
		if (projection_map) {
			projection_map->clear();
		}
		ClearCommonSubplanProjectionMaps(*op.children[i]);
	}
}

TEST_CASE("Common subplan consumers retain column pruning", "[optimizer][common_subplan]") {
	DuckDB db;
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE f AS SELECT i::BIGINT k, (i+1)::BIGINT v FROM range(1000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE d AS SELECT i::BIGINT k FROM range(10) t(i)"));
	const string query = R"(
		SELECT sum(a.v+b.v)
		FROM (SELECT f.k, f.v FROM f JOIN d USING (k)) a
		JOIN (SELECT f.k, f.v FROM f JOIN d USING (k)) b ON a.k=b.k
	)";
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::HUGEINT(110));
	con.BeginTransaction();
	Parser parser(*con.context);
	parser.ParseQuery(query);
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *con.context);
	auto plan = optimizer.Optimize(std::move(planner.plan));
	auto cte = FindCommonSubplanOperator(*plan, LogicalOperatorType::LOGICAL_MATERIALIZED_CTE);
	REQUIRE(cte);
	auto join = FindCommonSubplanOperator(*cte->children[1], LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
	REQUIRE(join);
	REQUIRE(join->GetColumnBindings().size() == 2);

	// Statistics can remove expressions and require the final column-lifetime pass to rebuild the maps.
	ClearCommonSubplanProjectionMaps(*plan);
	ColumnLifetimeAnalyzer column_lifetime(optimizer, *plan, true);
	column_lifetime.VisitOperator(*plan);
	REQUIRE(join->GetColumnBindings().size() == 2);
	con.Rollback();
}
