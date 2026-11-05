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
		CostModel cost_model(estimator);
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

TEST_CASE("Equivalent LEFT candidates share logical estimates in both cache orders", "[optimizer][join_order]") {
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
		CostModel costs(estimator);
		PlanEnumerator enumerator(graph, costs, graph.GetQueryGraphEdges());
		enumerator.InitLeafPlans();
		auto &plans = enumerator.GetPlans();
		auto &a = *plans.at(graph.set_manager.GetJoinRelation(RelationIndex(0)));
		auto &b = *plans.at(graph.set_manager.GetJoinRelation(RelationIndex(1)));
		auto &c = *plans.at(graph.set_manager.GetJoinRelation(RelationIndex(2)));
		auto &ab_set = graph.set_manager.Union(a.set, b.set);
		auto &ac_set = graph.set_manager.Union(a.set, c.set);
		if (warm) {
			estimator.EstimateCardinality(ab_set);
			estimator.EstimateCardinality(ac_set);
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
				node->cardinality = estimator.EstimateCardinality(set);
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
		// Candidate mutations cannot change logical group statistics.
		auto original = ab->cardinality;
		b.cardinality = CardinalityValue(0);
		a.cardinality = CardinalityValue(0);
		auto changed = estimator.EstimateCardinality(ab_set);
		REQUIRE_FALSE(original < changed);
		REQUIRE_FALSE(original > changed);
	}
	connection.Rollback();
}
static vector<unique_ptr<DPJoinNode>> EnumerateAlternatives(QueryGraphManager &graph, CardinalityEstimator &estimator,
                                                            CostModel &costs, idx_t mask) {
	unordered_set<RelationIndex> bindings;
	for (idx_t i = 0; i < graph.relation_manager.NumRelations(); i++) {
		if (mask & (idx_t(1) << i)) {
			bindings.insert(RelationIndex(i));
		}
	}
	auto &set = graph.set_manager.GetJoinRelation(bindings);
	vector<unique_ptr<DPJoinNode>> result;
	if (set.count == 1) {
		auto node = make_uniq<DPJoinNode>(set);
		node->cardinality = estimator.EstimateCardinality(set);
		node->cost = 0;
		result.push_back(std::move(node));
		return result;
	}
	for (idx_t left_mask = (mask - 1) & mask; left_mask; left_mask = (left_mask - 1) & mask) {
		auto left_plans = EnumerateAlternatives(graph, estimator, costs, left_mask);
		auto right_plans = EnumerateAlternatives(graph, estimator, costs, mask ^ left_mask);
		for (auto &left : left_plans) {
			for (auto &right : right_plans) {
				for (auto &connection : graph.GetQueryGraphEdges().GetConnections(left->set, right->set)) {
					auto &edge = connection.get();
					if (!graph.IsConnectionApplicable(edge, left->set, right->set) ||
					    !graph.IsJoinOrderCandidate(edge.join_operator, edge.generated_cross_product, left->set,
					                                right->set)) {
						continue;
					}
					auto rows = estimator.EstimateCardinality(set);
					auto node = make_uniq<DPJoinNode>(
					    set, edge.join_operator, edge.predicates, edge.generated_cross_product, left->set, right->set,
					    costs.ComputeCost(*left, *right, edge.join_operator, rows.GetDouble()));
					node->cardinality = rows;
					result.push_back(std::move(node));
				}
			}
		}
	}
	return result;
}

static double VerifySelectedCost(const DPJoinNode &node,
                                 const reference_map_t<JoinRelationSet, unique_ptr<DPJoinNode>> &plans) {
	if (node.is_leaf) {
		REQUIRE(node.cost == 0);
		return 0;
	}
	auto &left = *plans.at(node.left_set);
	auto &right = *plans.at(node.right_set);
	auto cost = VerifySelectedCost(left, plans) + VerifySelectedCost(right, plans) + node.cardinality.GetDouble();
	if (node.join_operator && node.join_operator->type == JoinOrderOperatorType::LEFT) {
		cost += right.cardinality.GetDouble();
	}
	CAPTURE(node.set.ToString());
	REQUIRE(node.cost == Approx(cost));
	return cost;
}

TEST_CASE("Equivalent mixed join plans share estimates and the cheapest plan survives", "[optimizer][join_order]") {
	DuckDB db;
	Connection connection(db);
	for (auto sql : {"CREATE TABLE a AS SELECT (i%100)::INT k FROM range(1000)t(i)",
	                 "CREATE TABLE b AS SELECT (i%100)::INT k FROM range(10)t(i)",
	                 "CREATE TABLE c AS SELECT (i%100)::INT k FROM range(10000)t(i)"}) {
		auto result = connection.Query(sql);
		REQUIRE_NO_FAIL(*result);
	}
	connection.BeginTransaction();
	for (auto query : {"SELECT * FROM (a SEMI JOIN b ON a.k=b.k) JOIN c ON a.k=c.k",
	                   "SELECT * FROM (a ANTI JOIN b ON a.k=b.k) JOIN c ON a.k=c.k",
	                   "SELECT * FROM (a LEFT JOIN b ON a.k=b.k) JOIN c ON a.k=c.k",
	                   "SELECT * FROM a LEFT JOIN b ON a.k=b.k LEFT JOIN c ON a.k=c.k"}) {
		CAPTURE(query);
		Parser parser(*connection.context);
		parser.ParseQuery(query);
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		JoinOrderOptimizer optimizer(*connection.context);
		QueryGraphManager graph(*connection.context);
		optional_ptr<LogicalOperator> logical_root = planner.plan.get();
		while (logical_root->children.size() == 1) {
			logical_root = logical_root->children[0].get();
		}
		REQUIRE(graph.Build(optimizer, *logical_root));
		REQUIRE(graph.relation_manager.NumRelations() == 3);
		auto &root = graph.set_manager.GetJoinRelation({RelationIndex(0), RelationIndex(1), RelationIndex(2)});
		CardinalityValue cold;
		for (bool root_first : {false, true}) {
			CardinalityEstimator estimator(graph.set_manager, graph.GetPredicateModel());
			CostModel costs(estimator);
			PlanEnumerator enumerator(graph, costs, graph.GetQueryGraphEdges());
			enumerator.InitLeafPlans();
			if (root_first) {
				auto estimate = estimator.EstimateCardinality(root);
				REQUIRE_FALSE(estimate < cold);
				REQUIRE_FALSE(estimate > cold);
			}
			auto alternatives = EnumerateAlternatives(graph, estimator, costs, 7);
			REQUIRE(alternatives.size() >= 2);
			double minimum = NumericLimits<double>::Maximum();
			cold = alternatives[0]->cardinality;
			for (auto &node : alternatives) {
				REQUIRE_FALSE(node->cardinality < cold);
				REQUIRE_FALSE(node->cardinality > cold);
				minimum = MinValue(minimum, node->cost);
			}
			REQUIRE(enumerator.SolveJoinOrder());
			auto &plans = enumerator.GetPlans();
			auto &selected = *plans.at(root);
			REQUIRE(selected.cost == Approx(minimum));
			VerifySelectedCost(selected, plans);
		}
	}
	connection.Rollback();
}

TEST_CASE("Selected costs remain current across exact and greedy enumeration", "[optimizer][join_order]") {
	DuckDB db;
	Connection connection(db);
	for (idx_t i = 0; i < 12; i++) {
		auto result = connection.Query("CREATE TABLE t" + to_string(i) + " AS SELECT i%17 AS k FROM range(" +
		                               to_string(20 + i * 73) + ") t(i)");
		REQUIRE_NO_FAIL(*result);
	}
	connection.BeginTransaction();
	for (idx_t count : {idx_t(5), idx_t(12)}) {
		for (idx_t threshold : {idx_t(1), idx_t(30)}) {
			CAPTURE(count, threshold);
			auto setting = connection.Query("SET approximate_join_order_threshold=" + to_string(threshold));
			REQUIRE_NO_FAIL(*setting);
			string sql = "SELECT count(*) FROM t0";
			for (idx_t i = 1; i < count; i++) {
				sql += " JOIN t" + to_string(i) + " ON t0.k=t" + to_string(i) + ".k";
			}
			Parser parser(*connection.context);
			parser.ParseQuery(sql);
			Planner planner(*connection.context);
			planner.CreatePlan(std::move(parser.statements[0]));
			JoinOrderOptimizer optimizer(*connection.context);
			QueryGraphManager graph(*connection.context);
			optional_ptr<LogicalOperator> logical_root = planner.plan.get();
			while (logical_root->children.size() == 1) {
				logical_root = logical_root->children[0].get();
			}
			REQUIRE(graph.Build(optimizer, *logical_root));
			CardinalityEstimator estimator(graph.set_manager, graph.GetPredicateModel());
			CostModel costs(estimator);
			PlanEnumerator enumerator(graph, costs, graph.GetQueryGraphEdges());
			enumerator.InitLeafPlans();
			REQUIRE(enumerator.SolveJoinOrder());
			unordered_set<RelationIndex> bindings;
			for (idx_t i = 0; i < count; i++) {
				bindings.insert(RelationIndex(i));
			}
			auto &root = graph.set_manager.GetJoinRelation(bindings);
			auto &plans = enumerator.GetPlans();
			if (count == 12 && threshold == 30) {
				// A complete 12-relation star exceeds the 10,000-pair budget.
				REQUIRE(plans.size() > 500);
				REQUIRE(plans.size() < (idx_t(1) << (count - 1)) + count - 1);
			}
			VerifySelectedCost(*plans.at(root), plans);
		}
	}
	connection.Rollback();
}
