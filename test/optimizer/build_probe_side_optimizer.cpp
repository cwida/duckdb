#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/build_probe_side_optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"

using namespace duckdb;

static optional_ptr<LogicalOperator> FindBuildProbeOperator(LogicalOperator &op, LogicalOperatorType type) {
	if (op.type == type) {
		return op;
	}
	for (auto &child : op.children) {
		auto result = FindBuildProbeOperator(*child, type);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

static unique_ptr<LogicalOperator> PlanBuildProbeQuery(Connection &connection, const string &query) {
	Parser parser(*connection.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	return std::move(planner.plan);
}

static void SetAsymmetricCrossProductEstimates(LogicalOperator &cross_product) {
	REQUIRE(cross_product.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	cross_product.children[0]->SetEstimatedCardinality(1);
	cross_product.children[1]->SetEstimatedCardinality(1000);
}

TEST_CASE("Build/probe optimization preserves correlated recursive domain build sides",
          "[optimizer][build_probe_side]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();

	const string correlated_query = R"(
		SELECT c.v, (
			SELECT count(*) FROM (
				WITH RECURSIVE t(i) AS (
					SELECT i FROM (VALUES (0)) a(i), range(1000) b(j)
					UNION ALL
					SELECT i + 1 FROM t WHERE i < c.v
				)
				SELECT * FROM t
			)
		)
		FROM (VALUES (2), (3)) c(v)
	)";
	auto recursive_plan = PlanBuildProbeQuery(connection, correlated_query);
	auto recursive_cte = FindBuildProbeOperator(*recursive_plan, LogicalOperatorType::LOGICAL_RECURSIVE_CTE);
	REQUIRE(recursive_cte);
	REQUIRE(recursive_cte->Cast<LogicalCTE>().correlated_columns.size() == 1);
	recursive_plan->ResolveOperatorTypes();
	auto anchor = std::move(recursive_cte->children[0]);
	auto anchor_cross = anchor.get();
	REQUIRE(anchor_cross->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	auto nested_cross = FindBuildProbeOperator(*anchor_cross->children[0], LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	REQUIRE(nested_cross);
	auto anchor_bindings = anchor_cross->GetColumnBindings();
	REQUIRE(anchor_bindings.size() == anchor_cross->types.size());
	vector<unique_ptr<Expression>> projection_expressions;
	for (idx_t column_idx = 0; column_idx < anchor_bindings.size(); column_idx++) {
		projection_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(anchor_cross->types[column_idx], anchor_bindings[column_idx]));
	}
	auto anchor_projection = make_uniq<LogicalProjection>(TableIndex(1000), std::move(projection_expressions));
	anchor_projection->children.push_back(std::move(anchor));
	recursive_cte->children[0] = std::move(anchor_projection);

	SetAsymmetricCrossProductEstimates(*anchor_cross);
	SetAsymmetricCrossProductEstimates(*nested_cross);
	auto anchor_domain = anchor_cross->children[1].get();
	auto nested_left = nested_cross->children[0].get();
	BuildProbeSideOptimizer recursive_optimizer(*connection.context, *recursive_plan);
	recursive_optimizer.VisitOperator(*recursive_plan);
	REQUIRE(anchor_cross->children[1].get() == anchor_domain);
	REQUIRE(nested_cross->children[1].get() == nested_left);
	auto correlated_result = connection.Query(correlated_query);
	REQUIRE_NO_FAIL(*correlated_result);
	CHECK_COLUMN(correlated_result, 0, {2, 3});
	CHECK_COLUMN(correlated_result, 1, {3000, 4000});

	const string recursive_query = R"(
		WITH RECURSIVE t(i, j) AS (
			SELECT i, j FROM (VALUES (1)) a(i), range(1000) b(j)
			UNION ALL
			SELECT i, j FROM t WHERE false
		)
		SELECT * FROM t
	)";
	auto uncorrelated_plan = PlanBuildProbeQuery(connection, recursive_query);
	auto uncorrelated_cte = FindBuildProbeOperator(*uncorrelated_plan, LogicalOperatorType::LOGICAL_RECURSIVE_CTE);
	REQUIRE(uncorrelated_cte);
	auto uncorrelated_cross =
	    FindBuildProbeOperator(*uncorrelated_cte->children[0], LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	REQUIRE(uncorrelated_cross);
	SetAsymmetricCrossProductEstimates(*uncorrelated_cross);
	auto uncorrelated_left = uncorrelated_cross->children[0].get();
	BuildProbeSideOptimizer uncorrelated_optimizer(*connection.context, *uncorrelated_plan);
	uncorrelated_optimizer.VisitOperator(*uncorrelated_plan);
	REQUIRE(uncorrelated_cross->children[1].get() == uncorrelated_left);

	auto ordinary_plan = PlanBuildProbeQuery(connection, "SELECT * FROM (VALUES (1)) a(i), range(1000) b(j)");
	auto ordinary_cross = FindBuildProbeOperator(*ordinary_plan, LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	REQUIRE(ordinary_cross);
	SetAsymmetricCrossProductEstimates(*ordinary_cross);
	auto ordinary_left = ordinary_cross->children[0].get();
	BuildProbeSideOptimizer ordinary_optimizer(*connection.context, *ordinary_plan);
	ordinary_optimizer.VisitOperator(*ordinary_plan);
	REQUIRE(ordinary_cross->children[1].get() == ordinary_left);

	connection.Rollback();
}
