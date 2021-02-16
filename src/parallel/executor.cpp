#include "duckdb/execution/executor.hpp"

#include "duckdb/execution/operator/helper/physical_execute.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/scan/physical_chunk_scan.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/parallel/task_context.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <algorithm>

namespace duckdb {

Executor::Executor(ClientContext &context) : context(context) {
}

Executor::~Executor() {
}

void Executor::Initialize(PhysicalOperator *plan) {
	Reset();

	thread_context = make_shared<ThreadContext>(context);
	TaskContext task;
	ExecutionContext econtext(context, *thread_context, task);
	physical_plan = plan;
	physical_state = physical_plan->GetOperatorState(econtext);

	context.profiler.Initialize(physical_plan);
	auto &scheduler = TaskScheduler::GetScheduler(context);
	this->producer = scheduler.CreateProducer();

	BuildPipelines(physical_plan, nullptr);

	this->total_pipelines = pipelines.size();

	// schedule pipelines that do not have dependents
	for (auto &pipeline : pipelines) {
		if (!pipeline->HasDependencies()) {
			pipeline->Schedule();
		}
	}

	// now execute tasks from this producer until all pipelines are completed
	while (completed_pipelines < total_pipelines) {
		unique_ptr<Task> task;
		while (scheduler.GetTaskFromProducer(*producer, task)) {
			task->Execute();
			task.reset();
		}
	}

	pipelines.clear();
	if (!exceptions.empty()) {
		// an exception has occurred executing one of the pipelines
		throw Exception(exceptions[0]);
	}
	context.profiler.Flush(thread_context->profiler);
}

void Executor::Reset() {
	delim_join_dependencies.clear();
	recursive_cte = nullptr;
	physical_plan = nullptr;
	physical_state = nullptr;
	completed_pipelines = 0;
	total_pipelines = 0;
	exceptions.clear();
	pipelines.clear();
}

void Executor::BuildPipelines(PhysicalOperator *op, Pipeline *parent) {
	if (op->IsSink()) {
		// operator is a sink, build a pipeline
		auto pipeline = make_unique<Pipeline>(*this, *producer);
		pipeline->sink = (PhysicalSink *)op;
		pipeline->sink_state = pipeline->sink->GetGlobalState(context);
		if (parent) {
			// the parent is dependent on this pipeline to complete
			parent->AddDependency(pipeline.get());
		}
		switch (op->type) {
		case PhysicalOperatorType::CREATE_TABLE_AS:
		case PhysicalOperatorType::INSERT:
		case PhysicalOperatorType::DELETE_OPERATOR:
		case PhysicalOperatorType::UPDATE:
		case PhysicalOperatorType::HASH_GROUP_BY:
		case PhysicalOperatorType::SIMPLE_AGGREGATE:
		case PhysicalOperatorType::PERFECT_HASH_GROUP_BY:
		case PhysicalOperatorType::WINDOW:
		case PhysicalOperatorType::ORDER_BY:
		case PhysicalOperatorType::RESERVOIR_SAMPLE:
		case PhysicalOperatorType::TOP_N:
		case PhysicalOperatorType::COPY_TO_FILE:
			// single operator, set as child
			pipeline->child = op->children[0].get();
			break;
		case PhysicalOperatorType::NESTED_LOOP_JOIN:
		case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
		case PhysicalOperatorType::HASH_JOIN:
		case PhysicalOperatorType::PIECEWISE_MERGE_JOIN:
		case PhysicalOperatorType::CROSS_PRODUCT:
			// regular join, create a pipeline with RHS source that sinks into this pipeline
			pipeline->child = op->children[1].get();
			// on the LHS (probe child), we recurse with the current set of pipelines
			BuildPipelines(op->children[0].get(), parent);
			break;
		case PhysicalOperatorType::DELIM_JOIN: {
			// duplicate eliminated join
			// create a pipeline with the duplicate eliminated path as source
			pipeline->child = op->children[0].get();
			break;
		}
		default:
			throw InternalException("Unimplemented sink type!");
		}
		// recurse into the pipeline child
		BuildPipelines(pipeline->child, pipeline.get());
		for (auto &dependency : pipeline->GetDependencies()) {
			auto dependency_cte = dependency->GetRecursiveCTE();
			if (dependency_cte) {
				pipeline->SetRecursiveCTE(dependency_cte);
			}
		}
		if (op->type == PhysicalOperatorType::DELIM_JOIN) {
			// for delim joins, recurse into the actual join
			// any pipelines in there depend on the main pipeline
			auto &delim_join = (PhysicalDelimJoin &)*op;
			// any scan of the duplicate eliminated data on the RHS depends on this pipeline
			// we add an entry to the mapping of (PhysicalOperator*) -> (Pipeline*)
			for (auto &delim_scan : delim_join.delim_scans) {
				delim_join_dependencies[delim_scan] = pipeline.get();
			}
			BuildPipelines(delim_join.join.get(), parent);
		}
		auto pipeline_cte = pipeline->GetRecursiveCTE();
		if (!pipeline_cte) {
			// regular pipeline: schedule it
			pipelines.push_back(move(pipeline));
		} else {
			// add it to the set of dependent pipelines in the CTE
			auto &cte = (PhysicalRecursiveCTE &)*pipeline_cte;
			cte.pipelines.push_back(move(pipeline));
		}
	} else {
		// operator is not a sink! recurse in children
		// first check if there is any additional action we need to do depending on the type
		switch (op->type) {
		case PhysicalOperatorType::DELIM_SCAN: {
			auto entry = delim_join_dependencies.find(op);
			D_ASSERT(entry != delim_join_dependencies.end());
			// this chunk scan introduces a dependency to the current pipeline
			// namely a dependency on the duplicate elimination pipeline to finish
			D_ASSERT(parent);
			parent->AddDependency(entry->second);
			break;
		}
		case PhysicalOperatorType::EXECUTE: {
			// EXECUTE statement: build pipeline on child
			auto &execute = (PhysicalExecute &)*op;
			BuildPipelines(execute.plan, parent);
			break;
		}
		case PhysicalOperatorType::RECURSIVE_CTE: {
			auto &cte_node = (PhysicalRecursiveCTE &)*op;
			// recursive CTE: we build pipelines on the LHS as normal
			BuildPipelines(op->children[0].get(), parent);
			// for the RHS, we gather all pipelines that depend on the recursive cte
			// these pipelines need to be rerun
			if (recursive_cte) {
				throw InternalException("Recursive CTE detected WITHIN a recursive CTE node");
			}
			recursive_cte = op;
			BuildPipelines(op->children[1].get(), parent);
			// re-order the pipelines such that they are executed in the correct order of dependencies
			for (idx_t i = 0; i < cte_node.pipelines.size(); i++) {
				auto &deps = cte_node.pipelines[i]->GetDependencies();
				for (idx_t j = i + 1; j < cte_node.pipelines.size(); j++) {
					if (deps.find(cte_node.pipelines[j].get()) != deps.end()) {
						// pipeline "i" depends on pipeline "j" but pipeline "i" is scheduled to be executed before
						// pipeline "j"
						std::swap(cte_node.pipelines[i], cte_node.pipelines[j]);
						i--;
						continue;
					}
				}
			}
			for (idx_t i = 0; i < cte_node.pipelines.size(); i++) {
				cte_node.pipelines[i]->ClearParents();
			}
			if (parent) {
				parent->SetRecursiveCTE(nullptr);
			}

			recursive_cte = nullptr;
			return;
		}
		case PhysicalOperatorType::RECURSIVE_CTE_SCAN: {
			if (!recursive_cte) {
				throw InternalException("Recursive CTE scan found without recursive CTE node");
			}
			if (parent) {
				// found a recursive CTE scan in a child pipeline
				// mark the child pipeline as recursive
				parent->SetRecursiveCTE(recursive_cte);
			}
			break;
		}
		default:
			break;
		}
		for (auto &child : op->children) {
			BuildPipelines(child.get(), parent);
		}
	}
}

vector<LogicalType> Executor::GetTypes() {
	D_ASSERT(physical_plan);
	return physical_plan->GetTypes();
}

void Executor::PushError(const string &exception) {
	lock_guard<mutex> elock(executor_lock);
	// interrupt execution of any other pipelines that belong to this executor
	context.interrupted = true;
	// push the exception onto the stack
	exceptions.push_back(exception);
}

void Executor::Flush(ThreadContext &tcontext) {
	lock_guard<mutex> elock(executor_lock);
	context.profiler.Flush(tcontext.profiler);
}

unique_ptr<DataChunk> Executor::FetchChunk() {
	D_ASSERT(physical_plan);

	TaskContext task;
	ExecutionContext econtext(context, *thread_context, task);

	auto chunk = make_unique<DataChunk>();
	// run the plan to get the next chunks
	physical_plan->InitializeChunkEmpty(*chunk);
	physical_plan->GetChunk(econtext, *chunk, physical_state.get());
	context.profiler.Flush(thread_context->profiler);
	return chunk;
}

} // namespace duckdb
