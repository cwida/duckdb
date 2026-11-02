#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/aggregate_rewrite.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

using namespace duckdb;

namespace {

static idx_t rewrite_calls;
static idx_t cost_calls;
static bool cost_saw_cardinality;

static unique_ptr<BoundAggregateExpression> BindAggregate(ClientContext &context, const char *name,
                                                          unique_ptr<Expression> child) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), name));
	auto function = entry.functions.GetFunctionByArguments(context, {child->GetReturnType()});
	FunctionBinder function_binder(context);
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));
	return function_binder.BindAggregateFunction(function, std::move(children));
}

static unique_ptr<Expression> StageGroup(const LogicalAggregate &op, TableIndex group_index, idx_t group_idx) {
	return make_uniq<BoundColumnRefExpression>(op.groups[group_idx]->GetReturnType(),
	                                           ColumnBinding(group_index, ProjectionIndex(group_idx)));
}

static unique_ptr<AggregateRewritePlan> RewriteDagAggregate(AggregateRewriteInput &input) {
	rewrite_calls++;
	auto &optimizer = *input.optimizer;
	auto &op = *input.op;
	const auto group_count = op.groups.size();
	auto plan = make_uniq<AggregateRewritePlan>();

	vector<unique_ptr<Expression>> distinct_groups;
	for (auto &group : op.groups) {
		distinct_groups.push_back(group->Copy());
	}
	distinct_groups.push_back(input.aggregate.GetChildren()[0]->Copy());
	auto distinct_group_index = optimizer.binder.GenerateTableIndex();
	auto distinct_aggregate_index = optimizer.binder.GenerateTableIndex();
	plan->stages.emplace_back(distinct_group_index, distinct_aggregate_index,
	                          vector<AggregateRewriteSource> {AggregateRewriteSource::Input()},
	                          std::move(distinct_groups), vector<unique_ptr<Expression>>());

	vector<unique_ptr<Expression>> max_groups;
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		max_groups.push_back(StageGroup(op, distinct_group_index, group_idx));
	}
	vector<unique_ptr<Expression>> max_aggregates;
	max_aggregates.push_back(
	    BindAggregate(input.context, "max",
	                  make_uniq<BoundColumnRefExpression>(
	                      LogicalType::BIGINT, ColumnBinding(distinct_group_index, ProjectionIndex(group_count)))));
	auto max_group_index = optimizer.binder.GenerateTableIndex();
	auto max_aggregate_index = optimizer.binder.GenerateTableIndex();
	plan->stages.emplace_back(max_group_index, max_aggregate_index,
	                          vector<AggregateRewriteSource> {AggregateRewriteSource::Stage(0)}, std::move(max_groups),
	                          std::move(max_aggregates));

	vector<unique_ptr<Expression>> min_groups;
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		min_groups.push_back(StageGroup(op, distinct_group_index, group_idx));
	}
	vector<unique_ptr<Expression>> min_aggregates;
	min_aggregates.push_back(
	    BindAggregate(input.context, "min",
	                  make_uniq<BoundColumnRefExpression>(
	                      LogicalType::BIGINT, ColumnBinding(distinct_group_index, ProjectionIndex(group_count)))));
	auto min_group_index = optimizer.binder.GenerateTableIndex();
	auto min_aggregate_index = optimizer.binder.GenerateTableIndex();
	plan->stages.emplace_back(min_group_index, min_aggregate_index,
	                          vector<AggregateRewriteSource> {AggregateRewriteSource::Stage(0)}, std::move(min_groups),
	                          std::move(min_aggregates));

	vector<unique_ptr<Expression>> final_groups;
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		final_groups.push_back(StageGroup(op, max_group_index, group_idx));
	}
	vector<unique_ptr<Expression>> final_aggregates;
	final_aggregates.push_back(
	    BindAggregate(input.context, "max",
	                  make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
	                                                      ColumnBinding(max_aggregate_index, ProjectionIndex(0)))));
	final_aggregates.push_back(
	    BindAggregate(input.context, "min",
	                  make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
	                                                      ColumnBinding(min_aggregate_index, ProjectionIndex(0)))));
	auto final_group_index = optimizer.binder.GenerateTableIndex();
	auto final_aggregate_index = optimizer.binder.GenerateTableIndex();
	plan->stages.emplace_back(
	    final_group_index, final_aggregate_index,
	    vector<AggregateRewriteSource> {AggregateRewriteSource::Stage(1), AggregateRewriteSource::Stage(2)},
	    std::move(final_groups), std::move(final_aggregates));

	plan->result_stage = 3;
	plan->result = optimizer.BindScalarFunction(
	    "+",
	    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
	                                        ColumnBinding(final_aggregate_index, ProjectionIndex(0))),
	    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
	                                        ColumnBinding(final_aggregate_index, ProjectionIndex(1))));
	return plan;
}

static bool CostDagAggregate(AggregateRewriteCostInput &input) {
	cost_calls++;
	cost_saw_cardinality = input.input_cardinality.IsValid();
	return true;
}

static void RegisterDagAggregate(Connection &con, const char *name = "test_dag_aggregate",
                                 AggregateRewritePolicy policy = AggregateRewritePolicy::MANDATORY,
                                 OptimizerType optimizer_type = OptimizerType::INVALID,
                                 aggregate_rewrite_cost_t cost = nullptr) {
	auto functions = CountFun::GetFunctions();
	auto function = *functions.GetFunctionByArguments(*con.context, {LogicalType::BIGINT});
	function.SetName(name);
	function.SetRewriteCallback(RewriteDagAggregate, policy, optimizer_type, cost);
	CreateAggregateFunctionInfo info(function);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

} // namespace

TEST_CASE("Aggregate rewrite plans support fan-out and branch joins", "[optimizer][aggregate_rewrite]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterDagAggregate(con);

	rewrite_calls = 0;
	auto result = con.Query("SELECT g, test_dag_aggregate(v::BIGINT) "
	                        "FROM (VALUES (1, 2), (1, 4), (1, 4), (2, 3), (NULL, 2), (NULL, 5)) t(g, v) "
	                        "GROUP BY g ORDER BY g NULLS LAST");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(1, 0).GetValue<int64_t>() == 6);
	REQUIRE(result->Collection().GetValue(1, 1).GetValue<int64_t>() == 6);
	REQUIRE(result->Collection().GetValue(1, 2).GetValue<int64_t>() == 7);
	REQUIRE(rewrite_calls == 1);

	rewrite_calls = 0;
	REQUIRE_NO_FAIL(*con.Query("SET enable_optimizer=false"));
	result = con.Query("SELECT test_dag_aggregate(v::BIGINT) FROM (VALUES (2), (4), (4)) t(v)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 6);
	REQUIRE(rewrite_calls == 1);
}

TEST_CASE("Aggregate rewrite policies own strategy selection", "[optimizer][aggregate_rewrite]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterDagAggregate(con, "test_costed_dag_aggregate", AggregateRewritePolicy::COST_BASED,
	                     OptimizerType::AGGREGATE_REUSE, CostDagAggregate);

	rewrite_calls = 0;
	cost_calls = 0;
	cost_saw_cardinality = false;
	auto result = con.Query("SELECT test_costed_dag_aggregate(i::BIGINT) FROM range(2, 5) t(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 6);
	REQUIRE(cost_calls == 1);
	REQUIRE(cost_saw_cardinality);
	REQUIRE(rewrite_calls == 1);

	REQUIRE_NO_FAIL(*con.Query("SET disabled_optimizers='aggregate_reuse'"));
	rewrite_calls = 0;
	cost_calls = 0;
	result = con.Query("SELECT test_costed_dag_aggregate(i::BIGINT) FROM range(2, 5) t(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 3);
	REQUIRE(cost_calls == 0);
	REQUIRE(rewrite_calls == 0);
}
