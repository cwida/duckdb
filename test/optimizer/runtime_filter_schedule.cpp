#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/execution/operator/join/physical_cross_product.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"

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
