#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_initialize_event.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

ExecutorTask::ExecutorTask(Executor &executor_p, shared_ptr<Event> event_p)
    : executor(executor_p), event(std::move(event_p)), context(executor_p.context) {
	executor.RegisterTask();
}

ExecutorTask::ExecutorTask(ClientContext &context_p, shared_ptr<Event> event_p, const PhysicalOperator &op_p)
    : executor(Executor::Get(context_p)), event(std::move(event_p)), op(&op_p), context(context_p) {
	thread_context = make_uniq<ThreadContext>(context_p);
	executor.RegisterTask();
}

ExecutorTask::~ExecutorTask() {
	if (thread_context) {
		executor.Flush(*thread_context);
	}
	thread_context.reset();
	event.reset();
	executor.UnregisterTask();
}

void ExecutorTask::Deschedule() {
	auto this_ptr = shared_from_this();
	executor.AddToBeRescheduled(this_ptr);
}

void ExecutorTask::Reschedule() {
	auto this_ptr = shared_from_this();
	executor.RescheduleTask(this_ptr);
}

namespace {
//! Traces a task of a pipeline outside its execute phase: initializing the sink, or a finalize step.
//! PipelineTask traces the execute phase itself.
struct StageTaskTrace {
	StageTaskTrace(ExecutorTask &task, const shared_ptr<Event> &event) {
		if (!event || dynamic_cast<PipelineTask *>(&task)) {
			return;
		}
		auto pipeline_event = dynamic_cast<BasePipelineEvent *>(event.get());
		if (!pipeline_event || !pipeline_event->pipeline || !pipeline_event->pipeline->trace) {
			return;
		}
		entry = pipeline_event->pipeline->trace.get();
		initialize = dynamic_cast<PipelineInitializeEvent *>(event.get()) != nullptr;
		tracer = TaskScheduler::GetScheduler(pipeline_event->pipeline->GetClientContext()).GetTracer();
		start_ns = tracer->Now();
		tracer->StageTaskStarted(*entry, initialize);
	}
	~StageTaskTrace() {
		if (entry) {
			tracer->StageTaskFinished(*entry, start_ns, initialize);
		}
	}
	optional_ptr<PipelineTraceEntry> entry;
	optional_ptr<PipelineTracer> tracer;
	int64_t start_ns = 0;
	bool initialize = false;
};
} // namespace

TaskExecutionResult ExecutorTask::Execute(TaskExecutionMode mode) {
	StageTaskTrace stage_trace(*this, event);
	try {
		if (thread_context) {
			TaskExecutionResult result;
			do {
				TaskNotifier task_notifier {context};
				thread_context->profiler.StartOperator(op);
				// to allow continuous profiling, always execute in small steps
				result = ExecuteTask(TaskExecutionMode::PROCESS_PARTIAL);
				thread_context->profiler.EndOperator(nullptr);
				executor.Flush(*thread_context);
			} while (mode == TaskExecutionMode::PROCESS_ALL && result == TaskExecutionResult::TASK_NOT_FINISHED);
			return result;
		} else {
			TaskNotifier task_notifier {context};
			auto result = ExecuteTask(mode);
			return result;
		}
	} catch (std::exception &ex) {
		executor.PushError(ErrorData(ex));
	} catch (...) { // LCOV_EXCL_START
		executor.PushError(ErrorData("Unknown exception in ExecutorTask::Execute"));
	} // LCOV_EXCL_STOP
	return TaskExecutionResult::TASK_ERROR;
}

} // namespace duckdb
