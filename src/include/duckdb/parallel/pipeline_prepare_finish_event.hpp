//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline_pre_finish_event.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parallel/base_pipeline_event.hpp"

namespace duckdb {
class Executor;

class PipelinePrepareFinishEvent : public BasePipelineEvent {
public:
	explicit PipelinePrepareFinishEvent(shared_ptr<Pipeline> pipeline);

public:
	void Schedule() override;
	void FinishEvent() override;
};

} // namespace duckdb
