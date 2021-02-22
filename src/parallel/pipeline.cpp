#include "duckdb/parallel/pipeline.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_context.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/main/database.hpp"

#include "duckdb/execution/operator/aggregate/physical_simple_aggregate.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"

namespace duckdb {

class PipelineTask : public Task {
public:
	explicit PipelineTask(Pipeline *pipeline_p) : pipeline(pipeline_p) {
	}

	TaskContext task;
	Pipeline *pipeline;

public:
	void Execute() override {
		pipeline->Execute(task);
		pipeline->FinishTask(task);
	}
};

Pipeline::Pipeline(Executor &executor_p, ProducerToken &token_p)
    : executor(executor_p), token(token_p), finished_tasks(0), total_tasks(0), finished_dependencies(0),
      finished(false), recursive_cte(nullptr) {
}

void Pipeline::Execute(TaskContext &task) {
	auto &client = executor.context;
	if (client.interrupted) {
		return;
	}
	if (parallel_state) {
		task.task_info[parallel_node] = parallel_state.get();
	}

	ThreadContext thread(client);
	ExecutionContext context(client, thread, task);
	try {
		auto state = child->GetOperatorState(context);
		auto lstate = sink->GetLocalSinkState(context);
		// incrementally process the pipeline
		DataChunk intermediate;
		child->InitializeChunkEmpty(intermediate);
		while (true) {
			child->GetChunk(context, intermediate, state.get());
			thread.profiler.StartOperator(sink);
			if (intermediate.size() == 0) {
				sink->Combine(context, *sink_state, *lstate);
				break;
			}
			sink->Sink(context, *sink_state, *lstate, intermediate);
			thread.profiler.EndOperator(nullptr);
		}
	} catch (std::exception &ex) {
		executor.PushError(ex.what());
	} catch (...) {
		executor.PushError("Unknown exception in pipeline!");
	}
	executor.Flush(thread);
}

void Pipeline::FinishTask(TaskContext &task) {
	ThreadContext thread(executor.context);
	ExecutionContext context(executor.context, thread, task);
	D_ASSERT(finished_tasks < total_tasks);
	idx_t current_finished = ++finished_tasks;
	if (current_finished == total_tasks) {
		try {
			sink->Finalize(*this, context, move(sink_state));
		} catch (std::exception &ex) {
			executor.PushError(ex.what());
		} catch (...) {
			executor.PushError("Unknown exception in Finalize!");
		}
		if (current_finished == total_tasks) {
			Finish();
		}
	}
}

void Pipeline::ScheduleSequentialTask() {
	auto &scheduler = TaskScheduler::GetScheduler(executor.context);
	auto task = make_unique<PipelineTask>(this);

	this->total_tasks = 1;
	scheduler.ScheduleTask(*executor.producer, move(task));
}

bool Pipeline::ScheduleOperator(PhysicalOperator *op) {
	switch (op->type) {
	case PhysicalOperatorType::UNNEST:
	case PhysicalOperatorType::FILTER:
	case PhysicalOperatorType::PROJECTION:
	case PhysicalOperatorType::HASH_JOIN:
	case PhysicalOperatorType::CROSS_PRODUCT:
	case PhysicalOperatorType::STREAMING_SAMPLE:
		// filter, projection or hash probe: continue in children
		return ScheduleOperator(op->children[0].get());
	case PhysicalOperatorType::TABLE_SCAN: {
		// we reached a scan: split it up into parts and schedule the parts
		auto &scheduler = TaskScheduler::GetScheduler(executor.context);
		auto &get = (PhysicalTableScan &)*op;
		if (!get.function.max_threads) {
			// table function cannot be parallelized
			return false;
		}
		D_ASSERT(get.function.init_parallel_state);
		D_ASSERT(get.function.parallel_state_next);
		idx_t max_threads = get.function.max_threads(executor.context, get.bind_data.get());
		if (max_threads > executor.context.db->NumberOfThreads()) {
			max_threads = executor.context.db->NumberOfThreads();
		}
		if (max_threads <= 1) {
			// table is too small to parallelize
			return false;
		}
		this->parallel_state = get.function.init_parallel_state(executor.context, get.bind_data.get());
		this->parallel_node = op;

		// launch a task for every thread
		this->total_tasks = max_threads;
		for (idx_t i = 0; i < max_threads; i++) {
			auto task = make_unique<PipelineTask>(this);
			scheduler.ScheduleTask(*executor.producer, move(task));
		}
		return true;
	}
	case PhysicalOperatorType::HASH_GROUP_BY: {
		// FIXME: parallelize scan of GROUP_BY HT
		return false;
	}
	default:
		// unknown operator: skip parallel task scheduling
		return false;
	}
}

void Pipeline::ClearParents() {
	for (auto &parent : parents) {
		parent->dependencies.erase(this);
	}
	for (auto &dep : dependencies) {
		dep->parents.erase(this);
	}
	parents.clear();
	dependencies.clear();
}

void Pipeline::Reset(ClientContext &context) {
	sink_state = sink->GetGlobalState(context);
	finished_tasks = 0;
	total_tasks = 0;
	finished = false;
}

void Pipeline::Schedule() {
	D_ASSERT(finished_tasks == 0);
	D_ASSERT(total_tasks == 0);
	D_ASSERT(finished_dependencies == dependencies.size());
	// check if we can parallelize this task based on the sink
	switch (sink->type) {
	case PhysicalOperatorType::SIMPLE_AGGREGATE: {
		auto &simple_aggregate = (PhysicalSimpleAggregate &)*sink;
		// simple aggregate: check if we can parallelize it
		if (!simple_aggregate.all_combinable) {
			// not all aggregates are parallelizable: switch to sequential mode
			break;
		}
		if (ScheduleOperator(sink->children[0].get())) {
			// all parallel tasks have been scheduled: return
			return;
		}
		break;
	}
	case PhysicalOperatorType::CREATE_TABLE_AS:
	case PhysicalOperatorType::ORDER_BY:
	case PhysicalOperatorType::RESERVOIR_SAMPLE:
	case PhysicalOperatorType::PERFECT_HASH_GROUP_BY: {
		// perfect hash aggregate can always be parallelized
		if (ScheduleOperator(sink->children[0].get())) {
			// all parallel tasks have been scheduled: return
			return;
		}
		break;
	}
	case PhysicalOperatorType::HASH_GROUP_BY: {
		auto &hash_aggr = (PhysicalHashAggregate &)*sink;
		if (!hash_aggr.all_combinable) {
			// not all aggregates are parallelizable: switch to sequential mode
			break;
		}
		if (ScheduleOperator(sink->children[0].get())) {
			// all parallel tasks have been scheduled: return
			return;
		}
		break;
	}
	case PhysicalOperatorType::CROSS_PRODUCT:
	case PhysicalOperatorType::HASH_JOIN: {
		// schedule build side of the join
		if (ScheduleOperator(sink->children[1].get())) {
			// all parallel tasks have been scheduled: return
			return;
		}
		break;
	}
	case PhysicalOperatorType::WINDOW: {
		// schedule child op
		if (ScheduleOperator(sink->children[0].get())) {
			// all parallel tasks have been scheduled: return
			return;
		}
		break;
	}
	default:
		break;
	}
	// could not parallelize this pipeline: push a sequential task instead
	ScheduleSequentialTask();
}

void Pipeline::AddDependency(Pipeline *pipeline) {
	this->dependencies.insert(pipeline);
	pipeline->parents.insert(this);
}

void Pipeline::CompleteDependency() {
	idx_t current_finished = ++finished_dependencies;
	D_ASSERT(current_finished <= dependencies.size());
	if (current_finished == dependencies.size()) {
		// all dependencies have been completed: schedule the pipeline
		Schedule();
	}
}

void Pipeline::Finish() {
	D_ASSERT(!finished);
	finished = true;
	// finished processing the pipeline, now we can schedule pipelines that depend on this pipeline
	for (auto &parent : parents) {
		// mark a dependency as completed for each of the parents
		parent->CompleteDependency();
	}
	executor.completed_pipelines++;
}

string Pipeline::ToString() const {
	string str = PhysicalOperatorToString(sink->type);
	auto node = this->child;
	while (node) {
		str = PhysicalOperatorToString(node->type) + " -> " + str;
		node = node->children[0].get();
	}
	return str;
}

void Pipeline::Print() const {
	Printer::Print(ToString());
}

} // namespace duckdb
