#include "catch.hpp"
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
