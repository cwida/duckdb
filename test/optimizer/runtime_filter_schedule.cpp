#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/execution/operator/aggregate/physical_ungrouped_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_cross_product.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

using namespace duckdb;

TEST_CASE("Runtime filter ordering respects transitive and sibling dependencies", "[runtime_filter_schedule]") {
	PipelineSchedule schedule;
	for (idx_t i = 0; i < 7; i++) {
		schedule.stages.emplace_back(PipelineScheduleStageType::EXECUTE, nullptr);
	}
	for (idx_t i = 1; i <= 4; i++) {
		schedule.stages[i].dependencies.push_back(i - 1);
	}
	// A sibling build must execute before this producer prepares its finalization.
	schedule.stages[2].dependencies.push_back(6);
	REQUIRE(schedule.TryAddDependency(5, 4));
	REQUIRE(schedule.DependsOn(5, 0));
	REQUIRE(schedule.TryAddDependency(5, 2));
	REQUIRE(schedule.stages[5].dependencies.size() == 1);
	REQUIRE_FALSE(schedule.TryAddDependency(6, 4));
	REQUIRE(schedule.stages[6].dependencies.empty());
	REQUIRE_FALSE(schedule.TryAddDependency(4, 4));
	REQUIRE_FALSE(schedule.HasCycle());
}

TEST_CASE("Sibling coordination uses build input cardinality", "[runtime_filter_schedule]") {
	DuckDB db(nullptr);
	Connection con(db);
	Executor executor(*con.context);
	PipelineBuildState state;
	for (idx_t estimate : {999999, 1000000, 1000001}) {
		CAPTURE(estimate);
		PhysicalPlan plan(Allocator::Get(*con.context));
		auto &probe = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 1);
		auto &build = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, estimate);
		auto &sibling_build = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 1000000);
		auto &join = plan.Make<PhysicalCrossProduct>(vector<LogicalType> {}, probe, build, 0);
		auto &sibling_join = plan.Make<PhysicalCrossProduct>(vector<LogicalType> {}, probe, sibling_build, 0);
		auto root = make_shared_ptr<MetaPipeline>(executor, state, nullptr);
		auto &child = root->CreateChildMetaPipeline(*root->GetBasePipeline(), join, MetaPipelineType::JOIN_BUILD);
		auto &sibling =
		    root->CreateChildMetaPipeline(*root->GetBasePipeline(), sibling_join, MetaPipelineType::JOIN_BUILD);
		vector<shared_ptr<MetaPipeline>> meta_pipelines;
		root->GetMetaPipelines(meta_pipelines, true, false);
		auto schedule = BuildPipelineSchedule(meta_pipelines);
		auto stage_index = [&](MetaPipeline &meta, PipelineScheduleStageType type) {
			for (idx_t i = 0; i < schedule->stages.size(); i++) {
				auto &stage = schedule->stages[i];
				if (stage.pipeline == meta.GetBasePipeline() && stage.type == type) {
					return i;
				}
			}
			throw InternalException("Missing test pipeline stage");
		};
		auto execute = stage_index(child, PipelineScheduleStageType::EXECUTE);
		auto prepare = stage_index(child, PipelineScheduleStageType::PREPARE_FINISH);
		auto finish = stage_index(child, PipelineScheduleStageType::FINISH);
		auto complete = stage_index(child, PipelineScheduleStageType::COMPLETE);
		auto sibling_execute = stage_index(sibling, PipelineScheduleStageType::EXECUTE);
		auto sibling_prepare = stage_index(sibling, PipelineScheduleStageType::PREPARE_FINISH);
		REQUIRE(schedule->DependsOn(prepare, sibling_execute) == (estimate >= 1000000));
		REQUIRE(schedule->DependsOn(finish, sibling_prepare) == (estimate >= 1000000));
		REQUIRE(schedule->DependsOn(sibling_prepare, execute));
		REQUIRE(schedule->DependsOn(stage_index(sibling, PipelineScheduleStageType::FINISH), prepare));
		REQUIRE(schedule->DependsOn(prepare, execute));
		REQUIRE(schedule->DependsOn(finish, prepare));
		REQUIRE(schedule->DependsOn(stage_index(*root, PipelineScheduleStageType::EXECUTE), complete));
		REQUIRE_FALSE(schedule->HasCycle());
	}
}

TEST_CASE("Small runtime filter builds exclude blocking and larger inputs", "[runtime_filter_schedule]") {
	DuckDB db(nullptr);
	Connection con(db);
	Executor executor(*con.context);
	PipelineBuildState state;
	PhysicalPlan plan(Allocator::Get(*con.context));
	auto &probe = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 100000);
	auto &small_input = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, DEFAULT_STANDARD_VECTOR_SIZE);
	auto &large_input = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, DEFAULT_STANDARD_VECTOR_SIZE + 1);
	auto &unknown_input = plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 0);
	auto &small_projection =
	    plan.Make<PhysicalProjection>(vector<LogicalType> {}, vector<unique_ptr<Expression>> {}, 1);
	small_projection.children.push_back(small_input);
	auto &large_projection =
	    plan.Make<PhysicalProjection>(vector<LogicalType> {}, vector<unique_ptr<Expression>> {}, 1);
	large_projection.children.push_back(large_input);
	auto &aggregate = plan.Make<PhysicalUngroupedAggregate>(vector<LogicalType> {}, vector<unique_ptr<Expression>> {},
	                                                        1, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	aggregate.children.push_back(unknown_input);
	auto &aggregate_projection =
	    plan.Make<PhysicalProjection>(vector<LogicalType> {}, vector<unique_ptr<Expression>> {}, 1);
	aggregate_projection.children.push_back(aggregate);
	auto &scan =
	    plan.Make<PhysicalTableScan>(vector<LogicalType> {}, BoundTableFunction(), nullptr, vector<LogicalType> {},
	                                 vector<ColumnIndex> {}, vector<idx_t> {}, vector<string> {}, nullptr, 100000,
	                                 ExtraOperatorInfo(), vector<Value> {}, virtual_column_map_t {})
	        .Cast<PhysicalTableScan>();
	scan.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
	auto &nested_join = plan.Make<PhysicalCrossProduct>(vector<LogicalType> {}, probe, scan, 100000);

	vector<pair<reference<PhysicalOperator>, bool>> builds {{small_input, true},      {large_input, false},
	                                                        {small_projection, true}, {large_projection, false},
	                                                        {aggregate, false},       {aggregate_projection, false}};
	vector<pair<bool, bool>> consumers {{false, false}, {true, false}, {false, true}};
	for (auto &build : builds) {
		for (auto &consumer : consumers) {
			const auto nested = consumer.first;
			const auto filtered = consumer.second;
			CAPTURE(build.first.get().type);
			CAPTURE(nested);
			CAPTURE(filtered);
			scan.table_filters.reset();
			if (filtered) {
				scan.table_filters = make_uniq<TableFilterSet>();
				scan.table_filters->PushFilter(
				    ProjectionIndex(0),
				    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, Value::BIGINT(0)));
			}
			LogicalComparisonJoin logical_join(JoinType::INNER);
			auto &producer = plan.Make<PhysicalHashJoin>(logical_join, probe, build.first.get(),
			                                             vector<JoinCondition> {}, JoinType::INNER, 100000)
			                     .Cast<PhysicalHashJoin>();
			producer.filter_pushdown = make_uniq<JoinFilterPushdownInfo>();
			producer.filter_pushdown->build_side_has_filter = false;
			JoinFilterPushdownFilter target;
			target.dynamic_filters = scan.dynamic_filters;
			target.estimated_scan_cardinality = 100000;
			producer.filter_pushdown->probe_info.push_back(std::move(target));
			auto consumer_pipeline = make_shared_ptr<Pipeline>(executor);
			state.SetPipelineSource(*consumer_pipeline, scan);
			auto producer_pipeline = make_shared_ptr<Pipeline>(executor);
			state.SetPipelineSink(*producer_pipeline, producer, 1);
			PipelineSchedule schedule;
			schedule.stages.emplace_back(PipelineScheduleStageType::EXECUTE, consumer_pipeline);
			schedule.stages.emplace_back(PipelineScheduleStageType::COMPLETE, producer_pipeline);
			if (nested) {
				state.SetPipelineSink(*consumer_pipeline, nested_join, 1);
				schedule.stages.emplace_back(PipelineScheduleStageType::COMPLETE, consumer_pipeline);
				schedule.stages[2].dependencies.push_back(0);
			}
			ScheduleRuntimeFilters(schedule);
			REQUIRE(schedule.DependsOn(0, 1) == ((!nested && !filtered) || build.second));
			REQUIRE_FALSE(schedule.HasCycle());
		}
	}
}
