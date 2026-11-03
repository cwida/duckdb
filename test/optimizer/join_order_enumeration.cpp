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

TEST_CASE("LEFT candidate estimates use input rows independently of domain cache warmup", "[optimizer][join_order]") {
	DuckDB db;
	Connection connection(db);
	for (auto sql : {"CREATE TABLE a AS SELECT (i%100)::INT k, i::INT v FROM range(1000)t(i)",
	                 "CREATE TABLE b AS SELECT (i%100)::INT k, (i*5)::INT v FROM range(200)t(i)",
	                 "CREATE TABLE c AS SELECT * FROM b"}) {
		auto result = connection.Query(sql);
		REQUIRE_NO_FAIL(*result);
	}
	connection.BeginTransaction();
	Parser parser(*connection.context);
	parser.ParseQuery("SELECT * FROM a LEFT JOIN b ON a.k=b.k AND a.v<b.v "
	                  "LEFT JOIN c ON a.k=c.k AND a.v<c.v");
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
	double cold_estimate = 0;
	for (auto warm : {false, true}) {
		CardinalityEstimator estimator(graph.set_manager, graph.GetPredicateModel());
		CostModel costs(graph, estimator);
		PlanEnumerator enumerator(graph, costs, graph.GetQueryGraphEdges());
		enumerator.InitLeafPlans();
		auto &plans = enumerator.GetPlans();
		auto &a = *plans.at(graph.set_manager.GetJoinRelation(RelationIndex(0)));
		auto &b = *plans.at(graph.set_manager.GetJoinRelation(RelationIndex(1)));
		auto &c = *plans.at(graph.set_manager.GetJoinRelation(RelationIndex(2)));
		auto &ab_set = graph.set_manager.Union(a.set, b.set);
		auto &ac_set = graph.set_manager.Union(a.set, c.set);
		if (warm) {
			estimator.EstimateCardinalityWithSet<double>(ab_set);
			estimator.EstimateCardinalityWithSet<double>(ac_set);
		}
		auto join = [&](DPJoinNode &left, DPJoinNode &right) {
			auto connections = graph.GetQueryGraphEdges().GetConnections(left.set, right.set);
			for (auto connection_ref : connections) {
				auto &edge = connection_ref.get();
				if (!graph.IsConnectionApplicable(edge, left.set, right.set) ||
				    !graph.IsJoinOrderCandidate(edge.join_operator, edge.generated_cross_product, left.set,
				                                right.set)) {
					continue;
				}
				REQUIRE(edge.join_operator);
				REQUIRE(edge.join_operator->type == JoinOrderOperatorType::LEFT);
				auto &set = graph.set_manager.Union(left.set, right.set);
				auto node = make_uniq<DPJoinNode>(set, edge.join_operator, edge.predicates,
				                                  edge.generated_cross_product, left.set, right.set, 0);
				node->cardinality = estimator.EstimateJoinCardinality(left, right, set, edge.join_operator);
				REQUIRE(node->cardinality.GetDouble() >= left.cardinality.GetDouble());
				return node;
			}
			throw InternalException("Expected a legal LEFT candidate");
		};
		auto ab = join(a, b);
		auto abc = join(*ab, c);
		auto ac = join(a, c);
		auto acb = join(*ac, b);
		REQUIRE(abc->cardinality.GetDouble() == Approx(acb->cardinality.GetDouble()));
		if (warm) {
			REQUIRE(abc->cardinality.GetDouble() == Approx(cold_estimate));
		} else {
			cold_estimate = abc->cardinality.GetDouble();
		}
		b.cardinality = CardinalityValue(0);
		REQUIRE(join(a, b)->cardinality.GetDouble() == a.cardinality.GetDouble());
		a.cardinality = CardinalityValue(0);
		REQUIRE(join(a, c)->cardinality.IsZero());
	}
	connection.Rollback();
}
