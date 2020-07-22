//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {
class Executor;

//! The Pipeline class represents an execution pipeline
class Pipeline : public std::enable_shared_from_this<Pipeline> {
	friend class Executor;

public:
	Pipeline(Executor &execution_context, idx_t maximum_threads = 1);

	Executor &executor;

public:
	//! Try to start working on this pipeline. Returns false if the pipeline is already fully occupied.
	bool TryWork();

	//! Execute the pipeline sequentially on a single thread
	void Execute();

	void AddDependency(Pipeline *pipeline);
	void EraseDependency(Pipeline *pipeline);
	bool HasDependencies() {
		return dependencies.size() != 0;
	}

	//! Finish executing this pipeline
	void Finish();

	string ToString() const;
	void Print() const;

private:
	std::mutex pipeline_lock;
	//! The child from which to pull chunks
	PhysicalOperator *child;
	//! The global sink state
	unique_ptr<GlobalOperatorState> sink_state;
	//! The sink (i.e. destination) for data; this is e.g. a hash table to-be-built
	PhysicalSink *sink;
	//! The parent pipelines (i.e. pipelines that are dependent on this pipeline to finish)
	unordered_set<Pipeline *> parents;
	//! The dependencies of this pipeline (the pipeline can only be started after the dependencies have finished
	//! executing)
	unordered_set<Pipeline *> dependencies;

	//! Whether or not the pipeline is finished executing
	bool finished;
	//! The current threads working on the pipeline
	idx_t current_threads;
	//! The maximum amount of threads that can work on the pipeline
	idx_t maximum_threads;
};

} // namespace duckdb
