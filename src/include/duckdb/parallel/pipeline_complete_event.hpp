//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline_complete_event.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parallel/event.hpp"

namespace duckdb {
class Executor;

struct PipelineTraceEntry;

class PipelineCompleteEvent : public Event {
public:
	PipelineCompleteEvent(Executor &executor, bool complete_pipeline_p,
	                      shared_ptr<PipelineTraceEntry> trace_p = nullptr);

	bool complete_pipeline;
	//! The trace of the meta pipeline's base pipeline, completed when this event finishes
	shared_ptr<PipelineTraceEntry> trace;

public:
	void Schedule() override;
	void FinalizeFinish() override;
};

} // namespace duckdb
