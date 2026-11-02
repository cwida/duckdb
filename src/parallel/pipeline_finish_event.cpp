#include "duckdb/parallel/pipeline_finish_event.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/parallel/executor_task.hpp"

namespace duckdb {

//! The PipelineFinishTask calls Finalize on the sink. Note that this is a single-threaded operation, but is executed
//! in a task to allow the Finalize call to block (e.g. for async I/O)
class PipelineFinishTask : public ExecutorTask {
public:
	explicit PipelineFinishTask(Pipeline &pipeline_p, shared_ptr<Event> event_p)
	    : ExecutorTask(pipeline_p.executor, std::move(event_p)), pipeline(pipeline_p) {
	}

	Pipeline &pipeline;

public:
	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		auto sink = pipeline.GetSink();
		InterruptState interrupt_state(shared_from_this());

#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
		if (debug_blocked_count < debug_blocked_target_count) {
			debug_blocked_count++;

			auto &callback_state = interrupt_state;
			std::thread rewake_thread([callback_state] {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				callback_state.Callback();
			});
			rewake_thread.detach();

			return TaskExecutionResult::TASK_BLOCKED;
		}
#endif
		// call finalize on the intermediate operators
		auto &&operators = pipeline.GetIntermediateOperators();
		for (; operator_idx < operators.size(); operator_idx++) {
			auto &op = operators[operator_idx].get();
			if (!op.RequiresOperatorFinalize()) {
				continue;
			}
			OperatorFinalizeInput op_finalize_input {*op.op_state, interrupt_state};
			auto op_state = op.OperatorFinalize(pipeline, *event, executor.context, op_finalize_input);
			if (op_state == OperatorFinalResultType::BLOCKED) {
				return TaskExecutionResult::TASK_BLOCKED;
			}
		}

		OperatorSinkFinalizeInput finalize_input {*sink->sink_state, interrupt_state};
		auto sink_state = sink->Finalize(pipeline, *event, executor.context, finalize_input);

		if (sink_state == SinkFinalizeType::BLOCKED) {
			return TaskExecutionResult::TASK_BLOCKED;
		}

		sink->sink_state->state = sink_state;
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "PipelineFinishTask";
	}

private:
	idx_t operator_idx = 0;
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	//! Debugging state: number of times blocked
	int debug_blocked_count = 0;
	//! Number of times the Finalize will block before actually returning data
	int debug_blocked_target_count = 1;
#endif
};

PipelineFinishEvent::PipelineFinishEvent(shared_ptr<Pipeline> pipeline_p) : BasePipelineEvent(std::move(pipeline_p)) {
	if (pipeline->trace) {
		pipeline->trace->has_finish_event = true;
	}
}

void PipelineFinishEvent::Schedule() {
	if (pipeline->trace) {
		auto &tracer = TaskScheduler::GetScheduler(pipeline->GetClientContext()).GetTracer();
		pipeline->trace->SetOnce(pipeline->trace->finalize_start_ns, tracer.Now());
	}
	vector<shared_ptr<Task>> tasks;
	tasks.push_back(make_uniq<PipelineFinishTask>(*pipeline, shared_from_this()));
	SetTasks(std::move(tasks));
}

void PipelineFinishEvent::FinishEvent() {
	if (pipeline->trace) {
		auto &tracer = TaskScheduler::GetScheduler(pipeline->GetClientContext()).GetTracer();
		pipeline->trace->SetOnce(pipeline->trace->finalize_end_ns, tracer.Now());
	}
}

} // namespace duckdb
