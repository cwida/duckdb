#include "duckdb/execution/executor.hpp"

#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/time_point.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/operator/helper/physical_result_collector.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline_complete_event.hpp"
#include "duckdb/parallel/pipeline_event.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/parallel/pipeline_finish_event.hpp"
#include "duckdb/parallel/pipeline_initialize_event.hpp"
#include "duckdb/parallel/pipeline_prepare_finish_event.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"
#include "duckdb/parallel/progress_verifier.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/thread_context.hpp"

#include <algorithm>
#include <chrono>

#ifdef __EMSCRIPTEN_PTHREADS__
#include <emscripten/threading.h>
#endif

namespace duckdb {

Executor::Executor(ClientContext &context) : context(context), executor_tasks(0), blocked_thread_time(0) {
}

Executor::~Executor() {
	D_ASSERT(Exception::UncaughtException() || executor_tasks == 0);
}

Executor &Executor::Get(ClientContext &context) {
	return context.GetExecutor();
}

void Executor::AddEvent(shared_ptr<Event> event) {
	lock_guard<mutex> elock(executor_lock);
	if (cancelled) {
		return;
	}
	events.push_back(std::move(event));
}

struct ScheduleEventData {
	ScheduleEventData(const vector<shared_ptr<MetaPipeline>> &meta_pipelines, vector<shared_ptr<Event>> &events,
	                  bool initial_schedule)
	    : meta_pipelines(meta_pipelines), events(events), initial_schedule(initial_schedule) {
	}

	const vector<shared_ptr<MetaPipeline>> &meta_pipelines;
	vector<shared_ptr<Event>> &events;
	bool initial_schedule;
};

static shared_ptr<Event> CreatePipelineScheduleEvent(const PipelineScheduleStage &stage, bool initial_schedule) {
	switch (stage.type) {
	case PipelineScheduleStageType::INITIALIZE:
		return make_shared_ptr<PipelineInitializeEvent>(stage.pipeline);
	case PipelineScheduleStageType::EXECUTE: {
		auto result = make_shared_ptr<PipelineEvent>(stage.pipeline);
		if (stage.pipeline->IsExternalInput()) {
			stage.pipeline->SetExternalInputEvent(result);
		}
		return result;
	}
	case PipelineScheduleStageType::PREPARE_FINISH:
		return make_shared_ptr<PipelinePrepareFinishEvent>(stage.pipeline);
	case PipelineScheduleStageType::FINISH:
		return make_shared_ptr<PipelineFinishEvent>(stage.pipeline);
	case PipelineScheduleStageType::COMPLETE:
		return make_shared_ptr<PipelineCompleteEvent>(stage.pipeline->executor, initial_schedule);
	default:
		throw InternalException("Unsupported pipeline schedule stage");
	}
}

void Executor::ScheduleEventsInternal(ScheduleEventData &event_data) {
	auto &events = event_data.events;
	D_ASSERT(events.empty());

	auto schedule = BuildPipelineSchedule(event_data.meta_pipelines);
	if (schedule->HasCycle()) {
		throw InternalException("Cyclic dependency in pipeline schedule");
	}
	if (event_data.initial_schedule && recursive_ctes.empty()) {
		ScheduleRuntimeFilters(*schedule);
	}
	events.reserve(schedule->stages.size());
	for (auto &stage : schedule->stages) {
		events.push_back(CreatePipelineScheduleEvent(stage, event_data.initial_schedule));
	}
	for (idx_t stage_idx = 0; stage_idx < schedule->stages.size(); stage_idx++) {
		for (auto dependency : schedule->stages[stage_idx].dependencies) {
			events[stage_idx]->AddDependency(*events[dependency]);
		}
	}
	for (auto &pipeline : schedule->initialize_on_schedule_pipelines) {
		pipeline.get().ResetSource(true);
	}

	// verify that we have no cyclic dependencies
	VerifyScheduledEvents(events);

	// schedule the pipelines that do not have dependencies
	for (auto &event : events) {
		if (!event->HasDependencies()) {
			event->Schedule();
			if (!event->HasTasks() && !event->IsFinished() && event->AutoFinishWithoutTasks()) {
				event->Finish();
			}
		}
	}
}

void Executor::ScheduleEvents(const vector<shared_ptr<MetaPipeline>> &meta_pipelines) {
	ScheduleEventData event_data(meta_pipelines, events, true);
	ScheduleEventsInternal(event_data);
}

void Executor::VerifyScheduledEvents(const vector<shared_ptr<Event>> &events) {
#ifdef DEBUG
	const idx_t count = events.size();
	vector<reference<Event>> vertices;
	vertices.reserve(count);
	for (const auto &event : events) {
		vertices.push_back(*event);
	}
	vector<bool> visited(count, false);
	vector<bool> recursion_stack(count, false);
	for (idx_t i = 0; i < count; i++) {
		VerifyScheduledEventsInternal(i, vertices, visited, recursion_stack);
	}
#endif
}

void Executor::VerifyScheduledEventsInternal(const idx_t vertex, const vector<reference<Event>> &vertices,
                                             vector<bool> &visited, vector<bool> &recursion_stack) {
	D_ASSERT(!recursion_stack[vertex]); // this vertex is in the recursion stack: circular dependency!
	if (visited[vertex]) {
		return; // early out: we already visited this vertex
	}

	auto &parents = vertices[vertex].get().GetParentsVerification();
	if (parents.empty()) {
		return; // early out: outgoing edges
	}

	// create a vector the indices of the adjacent events
	vector<idx_t> adjacent;
	const idx_t count = vertices.size();
	for (auto parent : parents) {
		idx_t i;
		for (i = 0; i < count; i++) {
			if (RefersToSameObject(vertices[i], parent)) {
				adjacent.push_back(i);
				break;
			}
		}
		D_ASSERT(i != count); // dependency must be in there somewhere
	}

	// mark vertex as visited and add to recursion stack
	visited[vertex] = true;
	recursion_stack[vertex] = true;

	// recurse into adjacent vertices
	for (const auto &i : adjacent) {
		VerifyScheduledEventsInternal(i, vertices, visited, recursion_stack);
	}

	// remove vertex from recursion stack
	recursion_stack[vertex] = false;
}

void Executor::AddRecursiveCTE(PhysicalOperator &rec_cte) {
	recursive_ctes.push_back(rec_cte);
}

void Executor::ReschedulePipelines(const vector<shared_ptr<MetaPipeline>> &pipelines_p,
                                   vector<shared_ptr<Event>> &events_p) {
	ScheduleEventData event_data(pipelines_p, events_p, false);
	ScheduleEventsInternal(event_data);
}

bool Executor::NextExecutor() {
	if (root_pipeline_idx >= root_pipelines.size()) {
		return false;
	}
	root_pipelines[root_pipeline_idx]->Reset();
	root_executor = make_uniq<PipelineExecutor>(context, *root_pipelines[root_pipeline_idx]);
	root_pipeline_idx++;
	return true;
}

void Executor::VerifyPipeline(Pipeline &pipeline) {
	D_ASSERT(!pipeline.ToString().empty());
	auto operators = pipeline.GetOperators();
	for (auto &other_pipeline : pipelines) {
		auto other_operators = other_pipeline->GetOperators();
		for (idx_t op_idx = 0; op_idx < operators.size(); op_idx++) {
			for (idx_t other_idx = 0; other_idx < other_operators.size(); other_idx++) {
				auto &left = operators[op_idx].get();
				auto &right = other_operators[other_idx].get();
				if (left.Equals(right)) {
					D_ASSERT(right.Equals(left));
				} else {
					D_ASSERT(!right.Equals(left));
				}
			}
		}
	}
}

void Executor::VerifyPipelines() {
#ifdef DEBUG
	for (auto &pipeline : pipelines) {
		VerifyPipeline(*pipeline);
	}
#endif
}

void Executor::Initialize(unique_ptr<PhysicalOperator> physical_plan_p) {
	Reset();
	owned_plan = std::move(physical_plan_p);
	InitializeInternal(*owned_plan);
}

void Executor::Initialize(PhysicalOperator &plan) {
	Reset();
	InitializeInternal(plan);
}

void Executor::InitializeInternal(PhysicalOperator &plan) {
	auto &scheduler = TaskScheduler::GetScheduler(context);
	{
		lock_guard<mutex> elock(executor_lock);
		physical_plan = &plan;

		this->profiler = ClientData::Get(context).profiler;
		this->producer = scheduler.CreateProducer();

		auto verify_progress = Settings::Get<DebugVerifyProgressSetting>(context);
		if (verify_progress != DebugProgressVerification::NONE) {
			progress_verifier = make_uniq<ProgressVerifier>(context, verify_progress,
			                                                Settings::Get<DebugVerifyProgressIgnoreSetting>(context));
		}

		// build and ready the pipelines
		PipelineBuildState state;
		auto root_pipeline = make_shared_ptr<MetaPipeline>(*this, state, nullptr);
		root_pipeline->Build(*physical_plan);

		// Resolve graph-dependent input modes after every pipeline and dependency has been constructed.
		vector<shared_ptr<MetaPipeline>> to_schedule;
		root_pipeline->GetMetaPipelines(to_schedule, true, true);
		state.ResolveExternalInputs(to_schedule);

		profiler->Initialize(plan);
		root_pipeline->Ready();

		// ready recursive cte pipelines too
		for (auto &rec_cte_ref : recursive_ctes) {
			auto &rec_cte = rec_cte_ref.get().Cast<PhysicalRecursiveCTE>();
			rec_cte.recursive_meta_pipeline->Ready();
		}

		// set root pipelines, i.e., all pipelines that end in the final sink
		root_pipeline->GetPipelines(root_pipelines, false);
		root_pipeline_idx = 0;

		// number of 'PipelineCompleteEvent's is equal to the number of meta pipelines, so we have to set it here
		total_pipelines = to_schedule.size();

		// collect all pipelines from the root pipelines (recursively) for the progress bar and verify them
		root_pipeline->GetPipelines(pipelines, true);

		// finally, verify and schedule
		VerifyPipelines();
		ScheduleEvents(to_schedule);
	}
}

void Executor::CancelTasks() {
	task.reset();
	{
		lock_guard<mutex> guard(result_buffer_lock);
		result_buffer.reset();
	}
	reference_map_t<Task, shared_ptr<Task>> to_destroy;
	{
		lock_guard<mutex> elock(executor_lock);
		// mark the query as cancelled so tasks will early-out
		cancelled = true;
		to_destroy = std::move(to_be_rescheduled_tasks);
		to_be_rescheduled_tasks.clear();
	}
	to_destroy.clear();
	// Drain all tasks first — they hold references to pipelines/events/states,
	// so those must stay alive until all tasks have completed
#ifndef DUCKDB_NO_THREADS
	if (producer) {
		auto &scheduler = TaskScheduler::GetScheduler(context);
		shared_ptr<Task> task_from_producer;
		while (true) {
			{
				annotated_unique_lock<annotated_mutex> lk(producer->producer_lock);
				if (executor_tasks == 0) {
					break;
				}
				if (!scheduler.GetTaskFromProducerLocked(*producer, task_from_producer)) {
					// Nothing to execute on this thread: wait until a task completes or is enqueued
					producer->producer_cv.wait(lk);
					continue;
				}
			}
			// Discard the dequeued task without executing it.
			task_from_producer.reset();
		}
	}
#else
	while (executor_tasks > 0) {
		WorkOnTasks();
	}
#endif
	// Now safe to destroy pipelines, events and states — no tasks reference them
	lock_guard<mutex> elock(executor_lock);
	for (auto &rec_cte_ref : recursive_ctes) {
		auto &rec_cte = rec_cte_ref.get().Cast<PhysicalRecursiveCTE>();
		rec_cte.recursive_meta_pipeline.reset();
	}
	pipelines.clear();
	root_pipelines.clear();
	to_be_rescheduled_tasks.clear();
	events.clear();
}

bool Executor::WorkOnTasks() {
	auto &scheduler = TaskScheduler::GetScheduler(context);

	bool did_work = false;
	shared_ptr<Task> task_from_producer;
	while (scheduler.GetTaskFromProducer(*producer, task_from_producer)) {
		did_work = true;
		auto res = task_from_producer->Execute(TaskExecutionMode::PROCESS_ALL);
		if (res == TaskExecutionResult::TASK_BLOCKED) {
			task_from_producer->Deschedule();
		}
		task_from_producer.reset();
	}
	return did_work;
}

void Executor::SignalTaskRescheduled(lock_guard<mutex> &) {
	task_reschedule.notify_one();
}

void Executor::UnregisterTask() {
#ifndef DUCKDB_NO_THREADS
	lock_guard<mutex> l(executor_lock);
	{
		const annotated_lock_guard<annotated_mutex> producer_lock(producer->producer_lock);
		executor_tasks--;
		producer->producer_cv.notify_all();
	}
	task_reschedule.notify_all();
#else
	executor_tasks--;
#endif
}

void Executor::WaitForTask() {
#ifndef DUCKDB_NO_THREADS
	static constexpr std::chrono::microseconds WAIT_TIME_MS = std::chrono::microseconds(WAIT_TIME * 1000);
	auto begin = TimePoint::Tick();
	std::unique_lock<mutex> l(executor_lock);
	auto end = TimePoint::Tick();
	auto blocked_micros = NumericCast<idx_t>(TimePoint::ElapsedMicros(begin, end));

	if (ExecutionIsFinished()) {
		blocked_thread_time += blocked_micros;
		return;
	}
	if (ResultCollectorIsBlocked()) {
		// Only the consumer's decision or pop lets the query progress, so waiting here is pointless
		blocked_thread_time += blocked_micros;
		return;
	}
	if (TaskScheduler::GetScheduler(context).GetTaskCountForProducer(*producer) > 0) {
		// A new task is available for the calling thread, the next step will make progress without waiting
		blocked_thread_time += blocked_micros;
		return;
	}
	// Nothing to run on this thread, all remaining tasks are either running on other threads or descheduled.
	// Wait (bounded), but wake up on task completion or reschedule.
	const auto wait_begin = TimePoint::Tick();
	task_reschedule.wait_for(l, WAIT_TIME_MS);
	const auto wait_micros = NumericCast<idx_t>(TimePoint::ElapsedMicros(wait_begin, TimePoint::Tick()));
	blocked_thread_time += blocked_micros + wait_micros;
#endif
}

void Executor::RescheduleTask(shared_ptr<Task> &task_p) {
	// This function will spin lock until the task provided is added to the to_be_rescheduled_tasks
	while (true) {
		lock_guard<mutex> l(executor_lock);
		if (cancelled) {
			return;
		}
		auto entry = to_be_rescheduled_tasks.find(*task_p);
		if (entry != to_be_rescheduled_tasks.end()) {
			auto &scheduler = TaskScheduler::GetScheduler(context);
			to_be_rescheduled_tasks.erase(*task_p);
			scheduler.ScheduleTask(GetToken(), task_p);
			SignalTaskRescheduled(l);
			break;
		}
	}
}

void Executor::AddToBeRescheduled(shared_ptr<Task> &task_p) {
	lock_guard<mutex> l(executor_lock);
	if (cancelled) {
		return;
	}
	if (to_be_rescheduled_tasks.find(*task_p) != to_be_rescheduled_tasks.end()) {
		return;
	}
	// Save the reference before move — evaluation order of operator[] key and assignment value is unspecified pre-C++17
	auto &task_ref = *task_p;
	to_be_rescheduled_tasks[task_ref] = std::move(task_p);
	// Only a result-sink park needs the consumer, so only a store that can park wakes it
	if (ResultStoreCanPark()) {
		task_reschedule.notify_all();
	}
}

bool Executor::ExecutionIsFinished() {
	return completed_pipelines >= total_pipelines || HasError();
}

QueryResultState Executor::ExecuteTask() {
	// Only executor should return NO_TASKS_AVAILABLE
	D_ASSERT(execution_result != QueryResultState::NO_TASKS_AVAILABLE);
	if (execution_result != QueryResultState::NOT_READY && ExecutionIsFinished()) {
		return execution_result;
	}
	if (completed_pipelines < total_pipelines) {
		if (!task) {
			TaskScheduler::GetScheduler(context).GetTaskFromProducer(*producer, task);
		}
		if (!task && !HasError()) {
#ifdef __EMSCRIPTEN_PTHREADS__
			// A positive sleep services the dlopen queue while another worker owns the task.
			emscripten_thread_sleep(1);
#endif
			return IdleState();
		}
		if (task) {
			// partially process the task
			auto result = task->Execute(TaskExecutionMode::PROCESS_PARTIAL);
			if (result == TaskExecutionResult::TASK_BLOCKED) {
				task->Deschedule();
				task.reset();
			} else if (result == TaskExecutionResult::TASK_FINISHED) {
				task.reset();
			} else if (result == TaskExecutionResult::TASK_ERROR) {
				if (!HasError()) {
					// This is very much unexpected, TASK_ERROR means this executor should have an Error
					throw InternalException("A task executed within Executor::ExecuteTask, from own producer, returned "
					                        "TASK_ERROR without setting error on the Executor");
				}
			}
		}
		if (!HasError()) {
			// we (partially) processed a task and no exceptions were thrown
			// give back control to the caller
			if (task && Settings::Get<SchedulerProcessPartialSetting>(context)) {
				auto &token = *task->token;
				TaskScheduler::GetScheduler(context).ScheduleTask(token, task);
				task.reset();
			}
			return QueryResultState::NOT_READY;
		}
		FailExecution();
	}
	return FinishExecution();
}

QueryResultState Executor::Poll() {
	D_ASSERT(execution_result != QueryResultState::NO_TASKS_AVAILABLE);
	if (execution_result != QueryResultState::NOT_READY && ExecutionIsFinished()) {
		return execution_result;
	}
	if (completed_pipelines < total_pipelines) {
		if (!HasError()) {
			return IdleState();
		}
		FailExecution();
	}
	return FinishExecution();
}

QueryResultState Executor::IdleState() {
	lock_guard<mutex> l(executor_lock);
	if (to_be_rescheduled_tasks.empty()) {
		return QueryResultState::NO_TASKS_AVAILABLE;
	}
	// At least one task is blocked
	if (ResultCollectorIsBlocked()) {
		return QueryResultState::READY;
	}
	return QueryResultState::BLOCKED;
}

void Executor::FailExecution() {
	execution_result = QueryResultState::EXECUTION_ERROR;
	// an exception has occurred executing one of the pipelines
	// we need to cancel all tasks associated with this executor
	CancelTasks();
	ThrowException();
}

QueryResultState Executor::FinishExecution() {
	D_ASSERT(!task);
	lock_guard<mutex> elock(executor_lock);
	if (progress_verifier && !HasError()) {
		auto progress_error = progress_verifier->Finalize(pipelines);
		if (!progress_error.empty()) {
			error_manager.PushError(ErrorData(ExceptionType::INVALID_INPUT, progress_error));
		}
	}
	pipelines.clear();
	NextExecutor();
	if (HasError()) { // LCOV_EXCL_START
		// an exception has occurred executing one of the pipelines
		execution_result = QueryResultState::EXECUTION_ERROR;
		ThrowException();
	} // LCOV_EXCL_STOP
	execution_result = QueryResultState::FINISHED;
	return execution_result;
}

void Executor::Reset() {
	lock_guard<mutex> elock(executor_lock);
	physical_plan = nullptr;
	cancelled = false;
	root_executor.reset();
	root_pipelines.clear();
	root_pipeline_idx = 0;
	completed_pipelines = 0;
	total_pipelines = 0;
	error_manager.Reset();
	progress_verifier.reset();
	pipelines.clear();
	events.clear();
	to_be_rescheduled_tasks.clear();
	execution_result = QueryResultState::NOT_READY;
}

shared_ptr<Pipeline> Executor::CreateChildPipeline(Pipeline &current, PhysicalOperator &op) {
	D_ASSERT(!current.operators.empty());
	D_ASSERT(op.IsSource());
	// found another operator that is a source, schedule a child pipeline
	// 'op' is the source, and the sink is the same
	auto child_pipeline = make_shared_ptr<Pipeline>(*this);
	child_pipeline->sink = current.sink;
	child_pipeline->source = &op;

	// the child pipeline has the same operators up until 'op'
	for (auto current_op : current.operators) {
		if (&current_op.get() == &op) {
			break;
		}
		child_pipeline->operators.push_back(current_op);
	}

	return child_pipeline;
}

vector<LogicalType> Executor::GetTypes() {
	D_ASSERT(physical_plan);
	return physical_plan->GetTypes();
}

void Executor::PushError(ErrorData exception) {
	// push the exception onto the stack
	error_manager.PushError(std::move(exception));
	InterruptPipelines();
}

void Executor::PushTaskError(const std::exception &ex) {
	error_manager.PushTaskError([&]() { return ErrorData(ex); });
	TryInterruptPipelines();
}

void Executor::PushTaskError(const char *message) {
	error_manager.PushTaskError([&]() { return ErrorData(message); });
	TryInterruptPipelines();
}

void Executor::TryInterruptPipelines() {
	try {
		InterruptPipelines();
	} catch (...) { // NOLINT
		// rescheduling blocked tasks can fail when memory runs out, and the error is recorded, so the main thread
		// sees it and cancels the remaining tasks
	}
}

void Executor::InterruptPipelines() {
	// interrupt execution of any other pipelines that belong to this executor
	context.interrupt_state = ClientInterruptState::INTERRUPTED;
	for (auto &pipeline : pipelines) {
		pipeline->FinishSourceAndPreventBlocking(context);
		pipeline->PreventSinkBlocking();
	}
}

bool Executor::HasError() {
	return error_manager.HasError();
}

ErrorData Executor::GetError() {
	return error_manager.GetError();
}

void Executor::ThrowException() {
	error_manager.ThrowException();
}

void Executor::Flush(ThreadContext &thread_context) {
	if (profiler) {
		profiler->Flush(thread_context.profiler);

		auto blocked_time = blocked_thread_time.load();
		profiler->SetBlockedTime(double(blocked_time) / 1000.0 / 1000.0);
	}
}

idx_t Executor::GetPipelinesProgress(ProgressData &progress) { // LCOV_EXCL_START
	lock_guard<mutex> elock(executor_lock);

	progress.done = 0;
	progress.total = 0;
	idx_t count_invalid = 0;
	for (auto &pipeline : pipelines) {
		ProgressData p;
		if (!pipeline->GetProgress(p)) {
			count_invalid++;
		} else {
			progress.Add(p);
		}
	}
	return count_invalid;
} // LCOV_EXCL_STOP

bool Executor::HasResultCollector() {
	return physical_plan->type == PhysicalOperatorType::RESULT_COLLECTOR;
}

bool Executor::HasStreamingResultCollector() {
	if (!HasResultCollector()) {
		return false;
	}
	auto &result_collector = physical_plan->Cast<PhysicalResultCollector>();
	return result_collector.IsStreaming();
}

void Executor::SetResultBuffer(shared_ptr<BufferedData> result_buffer_p) {
	lock_guard<mutex> guard(result_buffer_lock);
	result_buffer = std::move(result_buffer_p);
}

shared_ptr<BufferedData> Executor::GetResultBuffer() {
	lock_guard<mutex> guard(result_buffer_lock);
	return result_buffer;
}

bool Executor::ResultStoreCanPark() {
	if (!HasStreamingResultCollector()) {
		return false;
	}
	auto buffer = GetResultBuffer();
	// The store was settled on retained at submission: producers append and never park
	return !buffer || buffer->Lifetime() != ResultLifetime::RETAINED;
}

bool Executor::ResultCollectorIsBlocked() {
	// A store that cannot park never waits on the consumer, so the retained hot path skips the rest
	if (!ResultStoreCanPark()) {
		return false;
	}
	auto buffer = GetResultBuffer();
	if (buffer) {
		return buffer->WaitsOnConsumer();
	}
	auto &result_collector = physical_plan->Cast<PhysicalResultCollector>();
	// The sink state is published by a pipeline initialize task on a worker, under the
	// operator lock. Read it under the same lock, or readiness is reported before GetResult
	// has a state to fetch from
	lock_guard<mutex> guard(result_collector.lock);
	if (!result_collector.sink_state) {
		return false;
	}
	// A custom streaming collector has no parked-producer notion, and must never be waited on forever
	return result_collector.IsStreaming();
}

unique_ptr<QueryResult> Executor::GetResult() {
	D_ASSERT(HasResultCollector());
	auto &result_collector = physical_plan->Cast<PhysicalResultCollector>();
	D_ASSERT(result_collector.sink_state);
	return result_collector.GetResult(*result_collector.sink_state);
}

} // namespace duckdb
