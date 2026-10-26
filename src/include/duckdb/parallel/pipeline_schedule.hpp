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
//! Which dependencies participate in a graph traversal: only the scheduled (event) dependencies, or also the
//! analysis-only dependencies that model runtime completion ordering for cycle detection.
enum class PipelineScheduleGraph : uint8_t { SCHEDULED, WITH_ANALYSIS };

struct PipelineScheduleStage {
	PipelineScheduleStage(PipelineScheduleStageType type, shared_ptr<Pipeline> pipeline);

	PipelineScheduleStageType type;
	shared_ptr<Pipeline> pipeline;
	vector<idx_t> dependencies;
	//! Constraints that hold at runtime but must not gate event scheduling: a dataflow consumer's execute only
	//! completes once its producer has finalized, yet the consumer must be able to start and drain the exchange,
	//! otherwise producer backpressure deadlocks. Cycle analysis sees these, the event graph does not.
	vector<idx_t> analysis_dependencies;
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

	bool HasCycle(PipelineScheduleGraph graph = PipelineScheduleGraph::WITH_ANALYSIS) const;
	vector<PipelineScheduleEdge> GetCycle(PipelineScheduleGraph graph = PipelineScheduleGraph::WITH_ANALYSIS) const;
	bool DependsOn(idx_t dependent, idx_t dependency) const;
	bool TryAddDependency(idx_t dependent, idx_t dependency);
};

unique_ptr<PipelineSchedule> BuildPipelineSchedule(const vector<shared_ptr<MetaPipeline>> &meta_pipelines,
                                                   PipelineScheduleMode mode = PipelineScheduleMode::COMPLETE);
void ScheduleRuntimeFilters(PipelineSchedule &schedule);

} // namespace duckdb
