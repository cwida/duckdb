#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/join_order/plan_enumerator.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"

using namespace duckdb;

TEST_CASE("Connected hypergraphs can require original cross products", "[optimizer][join_order]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	for (auto sql : {"CREATE TABLE a AS SELECT i FROM range(7) t(i)", "CREATE TABLE b AS SELECT i FROM range(100) t(i)",
	                 "CREATE TABLE c AS SELECT i % 100 AS i, i AS j FROM range(10000) t(i)",
	                 "CREATE TABLE d AS SELECT i, i % 7 AS j FROM range(100) t(i)"}) {
		auto result = connection.Query(sql);
		REQUIRE_NO_FAIL(*result);
	}
	const string query = R"(
		SELECT count(*)
		FROM a, b,
		     (c SEMI JOIN
		        (VALUES (1), (3), (5), (7), (9), (11), (13), (15), (17), (19)) v(j)
		        ON c.j = v.j),
		     d
		WHERE a.i = d.j AND b.i = d.i AND c.i = d.i AND b.i = c.i
	)";
	for (auto threshold : {1, 30}) {
		auto setting = connection.Query("SET approximate_join_order_threshold=" + to_string(threshold));
		REQUIRE_NO_FAIL(*setting);
		Parser parser(*connection.context);
		parser.ParseQuery(query);
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		Optimizer optimizer(*planner.binder, *connection.context);
		FilterPushdown pushdown(optimizer);
		auto plan = pushdown.Rewrite(std::move(planner.plan));
		optional_ptr<LogicalOperator> root = plan.get();
		while (root->children.size() == 1) {
			root = root->children[0].get();
		}
		JoinOrderOptimizer join_optimizer(*connection.context);
		QueryGraphManager graph(*connection.context);
		REQUIRE(graph.Build(join_optimizer, *root));
		REQUIRE_FALSE(graph.RequiresCrossProduct());
		CardinalityEstimator estimator(graph.set_manager, graph.GetPredicateModel());
		CostModel cost_model(graph, estimator);
		PlanEnumerator enumerator(graph, cost_model, graph.GetQueryGraphEdges());
		enumerator.InitLeafPlans();
		// Result correctness alone cannot distinguish successful enumeration from the original-plan fallback.
		REQUIRE(enumerator.SolveJoinOrder());
		auto result = connection.Query(query);
		REQUIRE_NO_FAIL(*result);
		CHECK_COLUMN(result, 0, {10});
	}
	connection.Rollback();
}
