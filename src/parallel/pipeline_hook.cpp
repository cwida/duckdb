#include "duckdb/parallel/pipeline_hook.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_event.hpp"
#include "duckdb/parallel/pipeline_initialize_event.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

PipelineHook::PipelineHook(TaskScheduler &scheduler_p, PipelineTracer &tracer_p)
    : scheduler(scheduler_p), tracer(tracer_p), workers(PipelineTracer::MAX_WORKERS) {
}

PipelineHookMode PipelineHook::ParseMode(const string &mode_p) {
	auto mode_str = StringUtil::Lower(mode_p);
	if (mode_str.empty() || mode_str == "off") {
		return PipelineHookMode::OFF;
	}
	if (mode_str == "deferred") {
		return PipelineHookMode::DEFERRED;
	}
	if (mode_str == "immediate") {
		return PipelineHookMode::IMMEDIATE;
	}
	throw InvalidInputException("Unknown dbsched_scheduler_mode '%s': expected off, deferred, or immediate", mode_p);
}

void PipelineHook::SetMode(PipelineHookMode new_mode) {
	auto old_mode = mode.exchange(new_mode);
	if (old_mode == new_mode) {
		return;
	}
	if (new_mode != PipelineHookMode::OFF) {
		// wake workers that wait on the scheduler's semaphore, so they switch to waiting on the hook
		scheduler.Signal(scheduler.NumberOfThreads());
		return;
	}
	// turning the hook off: hand every queued task back to the shared queue and detach all workers
	vector<shared_ptr<Task>> to_schedule;
	{
		lock_guard<mutex> guard(lock);
		for (auto &entry : pipelines) {
			for (auto &pending : entry.second->pending) {
				to_schedule.push_back(std::move(pending.task));
			}
			entry.second->pending.clear();
		}
		for (auto &worker : workers) {
			if (worker.pipeline) {
				Detach(worker);
			}
		}
	}
	for (auto &task : to_schedule) {
		auto &token = *task->token;
		scheduler.ScheduleTask(token, std::move(task));
	}
	work_cv.notify_all();
}

optional_ptr<PipelineTraceEntry> PipelineHook::GetTraceEntry(Task &task) {
	auto executor_task = dynamic_cast<ExecutorTask *>(&task);
	if (!executor_task || !executor_task->event) {
		return nullptr;
	}
	auto pipeline_event = dynamic_cast<BasePipelineEvent *>(executor_task->event.get());
	if (!pipeline_event || !pipeline_event->pipeline || !pipeline_event->pipeline->trace) {
		return nullptr;
	}
	return pipeline_event->pipeline->trace.get();
}

bool PipelineHook::IsInitializeTask(Task &task) {
	auto executor_task = dynamic_cast<ExecutorTask *>(&task);
	return executor_task && dynamic_cast<PipelineInitializeEvent *>(executor_task->event.get());
}

bool PipelineHook::IsExecuteTask(Task &task) {
	auto executor_task = dynamic_cast<ExecutorTask *>(&task);
	return executor_task && dynamic_cast<PipelineEvent *>(executor_task->event.get());
}

idx_t PipelineHook::SubmittingWorker() {
	auto worker = PipelineTracer::GetCurrentWorker();
	if (worker.IsValid() && worker.GetIndex() < PipelineTracer::MAX_WORKERS) {
		return worker.GetIndex();
	}
	return DConstants::INVALID_INDEX;
}

PipelineHook::HookPipeline *PipelineHook::FindUnblockedFinalize(idx_t worker_idx, const HookPipeline *current) {
	for (auto &entry : pipelines) {
		auto &other = *entry.second;
		if (&other == current || other.removed || other.pending.empty()) {
			continue;
		}
		auto &front = other.pending.front();
		if (front.finalize_stage && front.unblocked_by == worker_idx && other.attached < GetCap(other)) {
			return &other;
		}
	}
	return nullptr;
}

PipelineHook::HookPipeline *PipelineHook::FindSinkFinalize(const HookPipeline &pipeline) {
	if (pipeline.sink_operator_id == DConstants::INVALID_INDEX) {
		return nullptr;
	}
	for (auto &entry : pipelines) {
		auto &other = *entry.second;
		if (&other == &pipeline || other.removed || other.pending.empty() ||
		    other.entry->query_id != pipeline.entry->query_id || other.sink_operator_id != pipeline.sink_operator_id) {
			continue;
		}
		if (other.pending.front().finalize_stage && other.attached < GetCap(other)) {
			return &other;
		}
	}
	return nullptr;
}

PipelineHook::HookPipeline &PipelineHook::GetPipeline(PipelineTraceEntry &entry) {
	auto it = pipelines.find(&entry);
	if (it != pipelines.end()) {
		return *it->second;
	}
	auto pipeline = make_uniq<HookPipeline>();
	pipeline->entry = entry.shared_from_this();
	pipeline->eligible_ns = tracer.Now();
	if (!entry.operator_ids.empty()) {
		pipeline->sink_operator_id = entry.operator_ids.back();
	}
	auto &result = *pipeline;
	pipelines[&entry] = std::move(pipeline);
	return result;
}

bool PipelineHook::TryEnqueue(shared_ptr<Task> &task) {
	if (!IsActive()) {
		return false;
	}
	auto entry = GetTraceEntry(*task);
	// DuckDB submits every pipeline's initialize task at query start, long before the pipeline can run: they are
	// left to the shared queue, where the query's client thread runs them (or a worker with nothing pending)
	if (!entry || IsInitializeTask(*task)) {
		return false;
	}
	{
		lock_guard<mutex> guard(lock);
		auto &pipeline = GetPipeline(*entry);
		auto finalize_stage = !IsExecuteTask(*task);
		pipeline.pending.push_back({std::move(task), tracer.Now(), finalize_stage, SubmittingWorker()});
		if (pipeline.current_step_tasks == 0) {
			pipeline.current_step_tasks = 1;
		}
	}
	work_cv.notify_all();
	return true;
}

void PipelineHook::TryEnqueue(vector<shared_ptr<Task>> &tasks) {
	if (!IsActive() || tasks.empty()) {
		return;
	}
	// the tasks of one batch belong to one event, hence to one pipeline
	auto entry = GetTraceEntry(*tasks[0]);
	if (!entry || IsInitializeTask(*tasks[0])) {
		return;
	}
	{
		lock_guard<mutex> guard(lock);
		auto &pipeline = GetPipeline(*entry);
		pipeline.current_step_tasks = tasks.size();
		entry->current_step_tasks = NumericCast<int64_t>(tasks.size());
		auto now = tracer.Now();
		auto finalize_stage = !IsExecuteTask(*tasks[0]);
		auto unblocked_by = SubmittingWorker();
		for (auto &task : tasks) {
			pipeline.pending.push_back({std::move(task), now, finalize_stage, unblocked_by});
		}
	}
	tasks.clear();
	work_cv.notify_all();
}

idx_t PipelineHook::WorkerCount() const {
	return MinValue<idx_t>(scheduler.NumberOfWorkerThreads(), workers.size());
}

void PipelineHook::ErasePipeline(const PipelineTraceEntry *entry) {
	auto it = pipelines.find(entry);
	if (it == pipelines.end()) {
		return;
	}
	for (auto &worker : workers) {
		if (worker.previous == it->second.get()) {
			worker.previous = nullptr;
		}
	}
	pipelines.erase(it);
}

PipelineHook::Worker &PipelineHook::GetWorker(idx_t worker_idx) {
	D_ASSERT(worker_idx < workers.size());
	return workers[worker_idx];
}

void PipelineHook::UpdateState(Worker &worker, int64_t now) {
	auto new_state = worker.busy ? WorkerState::BUSY : worker.pipeline ? WorkerState::ATTACHED_IDLE : WorkerState::FREE;
	if (worker.state_since_ns >= 0) {
		auto elapsed = NumericCast<uint64_t>(MaxValue<int64_t>(now - worker.state_since_ns, 0));
		switch (worker.state) {
		case WorkerState::BUSY:
			worker_times.busy_ns += elapsed;
			break;
		case WorkerState::ATTACHED_IDLE:
			worker_times.attached_idle_ns += elapsed;
			break;
		case WorkerState::FREE:
			worker_times.free_ns += elapsed;
			break;
		}
	}
	worker.state = new_state;
	worker.state_since_ns = now;
}

void PipelineHook::Attach(Worker &worker, HookPipeline &pipeline) {
	D_ASSERT(!worker.pipeline);
	worker.pipeline = &pipeline;
	pipeline.attached++;
	auto now = tracer.Now();
	UpdateState(worker, now);
	attach_log.Append({NumericCast<idx_t>(&worker - workers.data()), pipeline.entry->query_id,
	                   pipeline.entry->pipeline_id, true, now});
}

void PipelineHook::Detach(Worker &worker) {
	D_ASSERT(worker.pipeline);
	auto now = tracer.Now();
	attach_log.Append({NumericCast<idx_t>(&worker - workers.data()), worker.pipeline->entry->query_id,
	                   worker.pipeline->entry->pipeline_id, false, now});
	worker.previous = worker.pipeline;
	worker.pipeline->attached--;
	worker.pipeline = nullptr;
	UpdateState(worker, now);
}

optional_ptr<const std::pair<idx_t, idx_t>> PipelineHook::GetPolicyEntry(const HookPipeline &pipeline) const {
	auto query = policy_index.find(pipeline.entry->query_id);
	if (query == policy_index.end()) {
		return nullptr;
	}
	auto entry = query->second.find(pipeline.entry->pipeline_id);
	return entry == query->second.end() ? nullptr : &entry->second;
}

idx_t PipelineHook::GetCap(const HookPipeline &pipeline) const {
	// pipelines the policy does not list are not capped: they get the threads left over after the listed ones
	auto entry = GetPolicyEntry(pipeline);
	return entry ? entry->second : NumericLimits<idx_t>::Maximum();
}

vector<PipelineHook::HookPipeline *> PipelineHook::GetCandidates() {
	vector<HookPipeline *> result;
	for (auto &entry : pipelines) {
		auto &pipeline = *entry.second;
		if (!pipeline.removed && !pipeline.pending.empty() && GetCap(pipeline) > 0) {
			result.push_back(&pipeline);
		}
	}
	// the policy list first, in its order; then the pipelines it does not list, first come, first served: older
	// queries first, then the order the pipelines became eligible
	std::sort(result.begin(), result.end(), [&](const HookPipeline *a, const HookPipeline *b) {
		auto a_entry = GetPolicyEntry(*a);
		auto b_entry = GetPolicyEntry(*b);
		if (a_entry && b_entry) {
			return a_entry->first < b_entry->first;
		}
		if (a_entry || b_entry) {
			return bool(a_entry);
		}
		if (a->entry->query_id != b->entry->query_id) {
			return a->entry->query_id < b->entry->query_id;
		}
		if (a->eligible_ns != b->eligible_ns) {
			return a->eligible_ns < b->eligible_ns;
		}
		return a->entry->pipeline_id < b->entry->pipeline_id;
	});
	return result;
}

bool PipelineHook::AssignImmediate(Worker &worker) {
	for (auto pipeline : GetCandidates()) {
		// in immediate mode attached threads are busy ones, so attached counts the threads it is using
		if (pipeline->attached < GetCap(*pipeline)) {
			Attach(worker, *pipeline);
			return true;
		}
	}
	return false;
}

bool PipelineHook::TryGetTask(idx_t worker_idx, shared_ptr<Task> &task) {
	if (!IsActive()) {
		return false;
	}
	lock_guard<mutex> guard(lock);
	auto &worker = GetWorker(worker_idx);
	if (worker.state_since_ns < 0) {
		worker.state_since_ns = tracer.Now();
	}
	if (!worker.busy && (!worker.pipeline || worker.pipeline->pending.empty())) {
		// without waiting for a tick: finalize-stage work this worker's own tasks unblocked, then the finalize of a
		// sink its pipeline feeds
		auto next = FindUnblockedFinalize(worker_idx, worker.pipeline);
		if (!next && worker.pipeline) {
			next = FindSinkFinalize(*worker.pipeline);
		}
		if (next) {
			if (worker.pipeline) {
				Detach(worker);
			}
			Attach(worker, *next);
		}
	}
	if (worker.pipeline && worker.pipeline->pending.empty() && GetMode() == PipelineHookMode::IMMEDIATE) {
		// immediate mode: a thread is released as soon as its pipeline has nothing pending
		Detach(worker);
	}
	if (!worker.pipeline && GetMode() == PipelineHookMode::IMMEDIATE) {
		AssignImmediate(worker);
	}
	if (!worker.pipeline || worker.pipeline->pending.empty()) {
		return false;
	}
	auto now = tracer.Now();
	auto &pending = worker.pipeline->pending.front();
	auto &entry = *worker.pipeline->entry;
	entry.task_wait_ns += NumericCast<uint64_t>(MaxValue<int64_t>(now - pending.enqueued_ns, 0));
	entry.tasks_started++;
	worker.task_type = TaskTypeCode(pending.task->TaskType());
	worker.task_enqueued_ns = pending.enqueued_ns;
	worker.task_start_ns = now;
	task = std::move(pending.task);
	worker.pipeline->pending.pop_front();
	worker.pipeline->busy++;
	worker.busy = true;
	UpdateState(worker, now);
	return true;
}

void PipelineHook::TaskDone(idx_t worker_idx) {
	lock_guard<mutex> guard(lock);
	auto &worker = GetWorker(worker_idx);
	worker.busy = false;
	auto now = tracer.Now();
	UpdateState(worker, now);
	if (!worker.pipeline) {
		return;
	}
	auto &trace = *worker.pipeline->entry;
	task_log.Append({trace.query_id, trace.pipeline_id, worker.task_type, worker_idx, worker.task_enqueued_ns,
	                 worker.task_start_ns, now});
	auto &pipeline = *worker.pipeline;
	pipeline.busy--;
	if (pipeline.removed) {
		// its query is gone: release the thread, and the pipeline once no thread runs its tasks
		Detach(worker);
		if (pipeline.busy == 0 && pipeline.attached == 0) {
			ErasePipeline(pipeline.entry.get());
		}
	}
}

bool PipelineHook::IsAttached(idx_t worker_idx) {
	lock_guard<mutex> guard(lock);
	return GetWorker(worker_idx).pipeline != nullptr;
}

void PipelineHook::WaitForWork(idx_t worker_idx, int64_t timeout_us) {
	std::unique_lock<mutex> guard(lock);
	work_cv.wait_for(guard, std::chrono::microseconds(timeout_us));
}

void PipelineHook::Tick() {
	if (GetMode() != PipelineHookMode::DEFERRED) {
		return;
	}
	bool assigned_any = false;
	{
		lock_guard<mutex> guard(lock);
		// every waiting thread is free at a tick; busy threads keep their pipeline (no preemption)
		vector<idx_t> free_workers;
		auto worker_count = WorkerCount();
		for (idx_t i = 0; i < worker_count; i++) {
			auto &worker = workers[i];
			if (worker.busy) {
				continue;
			}
			if (worker.pipeline) {
				Detach(worker);
			}
			free_workers.push_back(i);
		}
		for (auto &entry : pipelines) {
			entry.second->assigned_at_tick = 0;
		}
		// free threads go down the candidate list, each pipeline up to its cap and its pending tasks,
		// preferring the threads it had before so that as few threads as possible change pipeline
		for (auto pipeline : GetCandidates()) {
			if (free_workers.empty()) {
				break;
			}
			auto cap = GetCap(*pipeline);
			auto room = pipeline->attached >= cap ? 0 : cap - pipeline->attached;
			auto wanted = MinValue<idx_t>(room, pipeline->pending.size());
			for (idx_t pass = 0; pass < 2 && wanted > 0; pass++) {
				for (idx_t i = 0; i < free_workers.size() && wanted > 0;) {
					auto &worker = workers[free_workers[i]];
					if (pass == 0 && worker.previous != pipeline) {
						i++;
						continue;
					}
					Attach(worker, *pipeline);
					pipeline->assigned_at_tick++;
					assigned_any = true;
					wanted--;
					free_workers.erase_at(i);
				}
			}
		}
	}
	// wake waiting workers only when the tick gave some of them a pipeline
	if (assigned_any) {
		work_cv.notify_all();
	}
}

void PipelineHook::SetPolicy(vector<PipelineHookPolicyEntry> entries) {
	lock_guard<mutex> guard(lock);
	policy = std::move(entries);
	policy_index.clear();
	for (idx_t i = 0; i < policy.size(); i++) {
		policy_index[policy[i].query_id][policy[i].pipeline_id] = std::make_pair(i, policy[i].cap);
	}
}

void PipelineHook::RemoveQuery(QueryTraceEntry &query, vector<shared_ptr<Task>> &dropped) {
	{
		lock_guard<mutex> guard(lock);
		for (auto &entry : query.pipelines) {
			auto it = pipelines.find(entry.get());
			if (it == pipelines.end()) {
				continue;
			}
			auto &pipeline = *it->second;
			pipeline.removed = true;
			for (auto &pending : pipeline.pending) {
				dropped.push_back(std::move(pending.task));
			}
			pipeline.pending.clear();
			// waiting threads are released now; busy ones when their task returns (TaskDone)
			for (auto &worker : workers) {
				if (worker.pipeline == &pipeline && !worker.busy) {
					Detach(worker);
				}
			}
			if (pipeline.busy == 0 && pipeline.attached == 0) {
				ErasePipeline(entry.get());
			}
		}
	}
	work_cv.notify_all();
}

unordered_map<const PipelineTraceEntry *, PipelineHook::Attachment> PipelineHook::GetAttachments() {
	unordered_map<const PipelineTraceEntry *, Attachment> result;
	lock_guard<mutex> guard(lock);
	for (idx_t i = 0; i < workers.size(); i++) {
		if (workers[i].pipeline) {
			result[workers[i].pipeline->entry.get()].workers.push_back(i);
		}
	}
	for (auto &entry : pipelines) {
		if (entry.second->assigned_at_tick > 0) {
			result[entry.first].assigned_at_tick = entry.second->assigned_at_tick;
		}
	}
	return result;
}

idx_t PipelineHook::GetFreeWorkers() {
	lock_guard<mutex> guard(lock);
	idx_t result = 0;
	auto worker_count = WorkerCount();
	for (idx_t i = 0; i < worker_count; i++) {
		if (!workers[i].pipeline && !workers[i].busy) {
			result++;
		}
	}
	return result;
}

void PipelineHook::NoteScheduled(Task &task) {
	auto entry = GetTraceEntry(task);
	if (entry) {
		entry->SetOnce(entry->eligible_ns, tracer.Now());
	}
}

PipelineHook::WorkerTimes PipelineHook::GetWorkerTimes() {
	lock_guard<mutex> guard(lock);
	auto result = worker_times;
	auto now = tracer.Now();
	for (idx_t i = 0; i < WorkerCount(); i++) {
		auto &worker = workers[i];
		if (worker.state_since_ns < 0) {
			continue;
		}
		auto elapsed = NumericCast<uint64_t>(MaxValue<int64_t>(now - worker.state_since_ns, 0));
		switch (worker.state) {
		case WorkerState::BUSY:
			result.busy_ns += elapsed;
			break;
		case WorkerState::ATTACHED_IDLE:
			result.attached_idle_ns += elapsed;
			break;
		case WorkerState::FREE:
			result.free_ns += elapsed;
			break;
		}
	}
	return result;
}

uint16_t PipelineHook::TaskTypeCode(const string &name) {
	auto entry = task_type_codes.find(name);
	if (entry != task_type_codes.end()) {
		return entry->second;
	}
	auto code = NumericCast<uint16_t>(task_type_names.size());
	task_type_names.push_back(name);
	task_type_codes[name] = code;
	return code;
}

idx_t PipelineHook::GetTaskRowCount() {
	lock_guard<mutex> guard(lock);
	return task_log.Size();
}

vector<vector<Value>> PipelineHook::GetTaskRows(idx_t offset, idx_t count) {
	vector<vector<Value>> result;
	lock_guard<mutex> guard(lock);
	auto end = MinValue<idx_t>(offset + count, task_log.Size());
	for (idx_t i = offset; i < end; i++) {
		auto &record = task_log[i];
		result.push_back({Value::UBIGINT(record.query_id), Value::UBIGINT(record.pipeline_id),
		                  Value(task_type_names[record.task_type]), Value::UBIGINT(record.worker),
		                  Value::BIGINT(record.enqueued_ns), Value::BIGINT(record.start_ns),
		                  Value::BIGINT(record.end_ns)});
	}
	return result;
}

void PipelineHook::ClearTaskLog() {
	lock_guard<mutex> guard(lock);
	task_log.Clear();
	attach_log.Clear();
}

idx_t PipelineHook::GetAttachmentRowCount() {
	lock_guard<mutex> guard(lock);
	return attach_log.Size();
}

vector<vector<Value>> PipelineHook::GetAttachmentRows(idx_t offset, idx_t count) {
	vector<vector<Value>> result;
	lock_guard<mutex> guard(lock);
	auto end = MinValue<idx_t>(offset + count, attach_log.Size());
	for (idx_t i = offset; i < end; i++) {
		auto &record = attach_log[i];
		result.push_back({Value::UBIGINT(record.worker), Value::UBIGINT(record.query_id),
		                  Value::UBIGINT(record.pipeline_id), Value(record.attach ? "attach" : "detach"),
		                  Value::BIGINT(record.t_ns)});
	}
	return result;
}

vector<vector<Value>> PipelineHook::GetStateRows() {
	vector<vector<Value>> result;
	lock_guard<mutex> guard(lock);
	for (auto &entry : pipelines) {
		auto &pipeline = *entry.second;
		auto &trace = *pipeline.entry;
		result.push_back({Value::UBIGINT(trace.query_id), Value::UBIGINT(trace.pipeline_id),
		                  Value::UBIGINT(pipeline.attached), Value::UBIGINT(pipeline.busy),
		                  Value::UBIGINT(pipeline.pending.size()), Value::UBIGINT(pipeline.current_step_tasks),
		                  Value::BIGINT(pipeline.eligible_ns), Value::UBIGINT(trace.source_chunks.load()),
		                  Value::UBIGINT(trace.source_rows.load())});
	}
	return result;
}

} // namespace duckdb
