//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline_schedule.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/reference_map.hpp"

namespace duckdb {

class MetaPipeline;
class Pipeline;

enum class PipelineScheduleStageType : uint8_t { INITIALIZE, EXECUTE, PREPARE_FINISH, FINISH, COMPLETE };
enum class PipelineScheduleMode : uint8_t { COMPLETE, ACTIVE_SUBSET };

struct PipelineScheduleStage {
	PipelineScheduleStage(PipelineScheduleStageType type, shared_ptr<Pipeline> pipeline);

	PipelineScheduleStageType type;
	shared_ptr<Pipeline> pipeline;
	vector<idx_t> dependencies;
};

struct PipelineScheduleExternalInputDependency {
	PipelineScheduleExternalInputDependency(idx_t dependent_p, idx_t dependency_index_p, Pipeline &consumer_p)
	    : dependent(dependent_p), dependency_index(dependency_index_p), consumer(consumer_p) {
	}

	idx_t dependent;
	idx_t dependency_index;
	reference<Pipeline> consumer;
};

struct PipelineScheduleEdge {
	PipelineScheduleEdge(idx_t dependent_p, idx_t dependency_p, optional_ptr<Pipeline> external_input_consumer_p)
	    : dependent(dependent_p), dependency(dependency_p), external_input_consumer(external_input_consumer_p) {
	}

	idx_t dependent;
	idx_t dependency;
	optional_ptr<Pipeline> external_input_consumer;
};

struct PipelineSchedule {
	vector<PipelineScheduleStage> stages;
	vector<reference<Pipeline>> initialize_on_schedule_pipelines;
	vector<PipelineScheduleExternalInputDependency> external_input_dependencies;

	bool HasCycle() const;
	vector<PipelineScheduleEdge> GetCycle() const;
	bool DependsOn(idx_t dependent, idx_t dependency) const;
	bool TryAddDependency(idx_t dependent, idx_t dependency);
};

unique_ptr<PipelineSchedule> BuildPipelineSchedule(const vector<shared_ptr<MetaPipeline>> &meta_pipelines,
                                                   PipelineScheduleMode mode = PipelineScheduleMode::COMPLETE);
void ScheduleRuntimeFilters(PipelineSchedule &schedule);

} // namespace duckdb
