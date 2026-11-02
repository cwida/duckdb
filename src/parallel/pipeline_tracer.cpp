#include "duckdb/parallel/pipeline_tracer.hpp"

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_hook.hpp"

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace duckdb {

static thread_local optional_idx current_worker;

const char *TraceStatusName(TraceStatus status) {
	switch (status) {
	case TraceStatus::UNSCHEDULED:
		return "unscheduled";
	case TraceStatus::NOT_STARTED:
		return "not_started";
	case TraceStatus::EXECUTING:
		return "executing";
	case TraceStatus::WAITING:
		return "waiting";
	case TraceStatus::FINALIZING:
		return "finalizing";
	case TraceStatus::FINISHED:
		return "finished";
	}
	return "unknown";
}

vector<idx_t> TraceWorkerMask::ToList() const {
	vector<idx_t> result;
	for (idx_t word = 0; word < TRACE_MAX_LISTED_WORKERS / 64; word++) {
		for (idx_t bit = 0; bit < 64; bit++) {
			if (bits[word] & (uint64_t(1) << bit)) {
				result.push_back(word * 64 + bit);
			}
		}
	}
	return result;
}

void PipelineTraceEntry::SetOnce(atomic<int64_t> &field, int64_t value) {
	int64_t expected = TRACE_NOT_SET;
	field.compare_exchange_strong(expected, value);
}

void PipelineTraceEntry::SetMax(atomic<int64_t> &field, int64_t value) {
	auto current = field.load();
	while (current < value && !field.compare_exchange_weak(current, value)) {
	}
}

int64_t PipelineTraceEntry::FinishedAt() const {
	// a base pipeline is done when its meta pipeline completes, after any finalize steps its sink added; a pipeline
	// with its own finish event when that event ends; any other pipeline when its execute phase ends
	if (has_complete_event.load()) {
		return complete_ns.load();
	}
	if (has_finish_event.load()) {
		return finalize_end_ns.load();
	}
	return execute_end_ns.load();
}

PipelineTracer::PipelineTracer()
    : epoch(std::chrono::steady_clock::now()), worker_pipelines(new atomic<PipelineTraceEntry *>[MAX_WORKERS]) {
	for (idx_t i = 0; i < MAX_WORKERS; i++) {
		worker_pipelines[i] = nullptr;
	}
}

PipelineTracer::~PipelineTracer() {
	SetTickInterval(0);
}

void PipelineTracer::SetCurrentWorker(idx_t worker_idx) {
	current_worker = worker_idx;
}

optional_idx PipelineTracer::GetCurrentWorker() {
	return current_worker;
}

int64_t PipelineTracer::Now() const {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - epoch).count();
}

void PipelineTracer::SetTickInterval(idx_t interval_us) {
	lock_guard<mutex> config_guard(config_lock);
	if (interval_us == tick_interval_us.load() && (interval_us == 0) == !tick_thread) {
		return;
	}
	// stop the running thread, if any, then start one with the new interval
	if (tick_thread) {
		{
			lock_guard<mutex> guard(thread_lock);
			stop_thread = true;
		}
		thread_cv.notify_all();
		tick_thread->join();
		tick_thread.reset();
	}
	tick_interval_us = interval_us;
	if (interval_us == 0) {
		return;
	}
	stop_thread = false;
	tick_thread = make_uniq<std::thread>([this]() { TickLoop(); });
}

void PipelineTracer::TickLoop() {
#if defined(__linux__)
	// Linux delays a sleeping thread's wakeup by up to its timer slack (50 microseconds by default): make ticks exact
	prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
#endif
	auto interval = std::chrono::microseconds(tick_interval_us.load());
	auto next = std::chrono::steady_clock::now() + interval;
	while (true) {
		{
			std::unique_lock<mutex> guard(thread_lock);
			if (thread_cv.wait_until(guard, next, [this]() { return stop_thread; })) {
				return;
			}
		}
		auto tick_start_ns = Now();
		if (hook) {
			hook->Tick();
		}
		TakeSnapshot(tick_start_ns);
		// ticks stay on a fixed grid: skip any tick that was missed instead of drifting
		next += interval;
		auto now = std::chrono::steady_clock::now();
		while (next <= now) {
			next += interval;
		}
	}
}

TraceStatus PipelineTracer::GetStatus(const PipelineTraceEntry &entry) {
	if (entry.FinishedAt() != TRACE_NOT_SET) {
		return TraceStatus::FINISHED;
	}
	if (entry.finalize_start_ns.load() != TRACE_NOT_SET) {
		return TraceStatus::FINALIZING;
	}
	if (entry.execute_end_ns.load() != TRACE_NOT_SET) {
		return TraceStatus::WAITING;
	}
	if (entry.first_task_ns.load() != TRACE_NOT_SET) {
		return TraceStatus::EXECUTING;
	}
	if (entry.scheduled_ns.load() != TRACE_NOT_SET) {
		return TraceStatus::NOT_STARTED;
	}
	return TraceStatus::UNSCHEDULED;
}

void PipelineTracer::TakeSnapshot(int64_t tick_start_ns) {
	lock_guard<mutex> guard(lock);
	auto now = Now();
	auto tick = next_tick++;

	// which workers are on which pipeline
	unordered_map<PipelineTraceEntry *, vector<idx_t>> workers_by_pipeline;
	idx_t busy_workers = 0;
	for (idx_t i = 0; i < MAX_WORKERS; i++) {
		auto entry = worker_pipelines[i].load();
		if (entry) {
			workers_by_pipeline[entry].push_back(i);
			busy_workers++;
		}
	}

	// with the hook on, a pipeline's threads are the workers attached to it
	bool hook_active = hook && hook->IsActive();
	unordered_map<const PipelineTraceEntry *, PipelineHook::Attachment> attachments;
	if (hook_active) {
		attachments = hook->GetAttachments();
	}

	idx_t busy_total = 0;
	idx_t active_pipelines = 0;
	auto snapshot_queries = [&](const vector<shared_ptr<QueryTraceEntry>> &queries) {
		for (auto &query : queries) {
			for (auto &entry_ptr : query->pipelines) {
				auto &entry = *entry_ptr;
				auto scheduled = entry.scheduled_ns.load();
				if (scheduled == TRACE_NOT_SET) {
					continue;
				}
				// a pipeline that finished appears once more, at the first tick after it finished
				auto finished_at = entry.FinishedAt();
				if (finished_at != TRACE_NOT_SET && finished_at <= last_tick_ns) {
					continue;
				}
				IntervalRow row {};
				row.tick = tick;
				row.t_ns = now;
				row.query_id = entry.query_id;
				row.pipeline_id = entry.pipeline_id;
				row.status = GetStatus(entry);
				row.running = entry.running_threads.load();
				row.assigned = 0;
				if (hook_active) {
					auto attached = attachments.find(entry_ptr.get());
					row.threads = 0;
					if (attached != attachments.end()) {
						for (auto worker : attached->second.workers) {
							row.workers.Set(worker);
						}
						row.threads = attached->second.workers.size();
						row.assigned = attached->second.assigned_at_tick;
					}
				} else {
					auto workers = workers_by_pipeline.find(entry_ptr.get());
					if (workers != workers_by_pipeline.end()) {
						for (auto worker : workers->second) {
							row.workers.Set(worker);
						}
					}
					row.threads = row.running;
				}
				row.source_chunks = entry.source_chunks.load();
				row.source_rows = entry.source_rows.load();
				row.sink_rows = entry.sink_rows.load();
				row.thread_ns = entry.thread_ns.load();
				row.finalize_thread_ns = entry.finalize_thread_ns.load();
				busy_total += row.running;
				if (row.status != TraceStatus::FINISHED) {
					active_pipelines++;
				}
				interval_rows.Append(row);
			}
		}
	};
	snapshot_queries(active_queries);
	snapshot_queries(recently_finished);
	recently_finished.clear();

	TickRow tick_row {};
	tick_row.tick = tick;
	tick_row.t_ns = now;
	tick_row.busy_workers = busy_workers;
	tick_row.busy_external = external_running.load();
	tick_row.active_queries = active_queries.size();
	tick_row.active_pipelines = active_pipelines;
	tick_row.free_workers = hook_active ? hook->GetFreeWorkers() : 0;
	tick_row.untraced_tasks = hook ? hook->GetUntracedTasks() : 0;
	PipelineHook::WorkerTimes times;
	if (hook) {
		times = hook->GetWorkerTimes();
	}
	tick_row.busy_ns = times.busy_ns;
	tick_row.attached_idle_ns = times.attached_idle_ns;
	tick_row.free_ns = times.free_ns;
	tick_row.tick_work_ns = Now() - tick_start_ns;
	tick_rows.Append(tick_row);
	last_tick_ns = now;
}

shared_ptr<QueryTraceEntry> PipelineTracer::RegisterQuery(Executor &executor,
                                                          const vector<shared_ptr<Pipeline>> &pipelines, string tag,
                                                          string query) {
	auto result = make_shared_ptr<QueryTraceEntry>();
	result->query_id = next_query_id++;
	result->tag = std::move(tag);
	result->query = std::move(query);
	result->submitted_ns = Now();

	// pipeline ids follow the executor's pipeline order, which is deterministic for a plan
	unordered_map<const Pipeline *, idx_t> pipeline_ids;
	for (idx_t i = 0; i < pipelines.size(); i++) {
		pipeline_ids[pipelines[i].get()] = i;
	}
	for (idx_t i = 0; i < pipelines.size(); i++) {
		auto &pipeline = *pipelines[i];
		auto entry = make_shared_ptr<PipelineTraceEntry>();
		entry->query_id = result->query_id;
		entry->pipeline_id = i;
		for (auto &op_ref : pipeline.GetOperators()) {
			auto &op = op_ref.get();
			entry->operator_ids.push_back(op.operator_id);
			entry->operator_names.push_back(op.GetName());
		}
		auto source = pipeline.GetSource();
		if (source) {
			entry->source_estimated_cardinality = source->estimated_cardinality;
		}
		for (auto &dependency : pipeline.GetAllDependencies()) {
			auto id = pipeline_ids.find(dependency.get());
			if (id != pipeline_ids.end()) {
				entry->dependencies.push_back(id->second);
			}
		}
		pipeline.trace = entry;
		result->pipelines.push_back(std::move(entry));
	}

	lock_guard<mutex> guard(lock);
	active_queries.push_back(result);
	return result;
}

void PipelineTracer::FinishQuery(QueryTraceEntry &query, bool error) {
	lock_guard<mutex> guard(lock);
	for (idx_t i = 0; i < active_queries.size(); i++) {
		if (active_queries[i].get() != &query) {
			continue;
		}
		query.finished_ns = Now();
		query.error = error;
		recently_finished.push_back(active_queries[i]);
		finished_queries.push_back(std::move(active_queries[i]));
		active_queries.erase_at(i);
		return;
	}
}

void PipelineTracer::TaskStarted(PipelineTraceEntry &entry) {
	entry.SetOnce(entry.first_task_ns, Now());
	entry.running_threads++;
	auto worker = current_worker;
	if (worker.IsValid() && worker.GetIndex() < MAX_WORKERS) {
		worker_pipelines[worker.GetIndex()] = &entry;
	} else {
		external_running++;
	}
}

void PipelineTracer::TaskFinished(PipelineTraceEntry &entry, int64_t start_ns) {
	auto worker = current_worker;
	if (worker.IsValid() && worker.GetIndex() < MAX_WORKERS) {
		worker_pipelines[worker.GetIndex()] = nullptr;
	} else {
		external_running--;
	}
	entry.running_threads--;
	entry.thread_ns += NumericCast<uint64_t>(MaxValue<int64_t>(Now() - start_ns, 0));
}

void PipelineTracer::TaskCompleted(PipelineTraceEntry &entry) {
	auto finished = ++entry.tasks_finished;
	auto task_count = entry.task_count.load();
	if (task_count != TRACE_NOT_SET && finished >= NumericCast<idx_t>(task_count)) {
		entry.SetOnce(entry.execute_end_ns, Now());
	}
}

void PipelineTracer::StageTaskStarted(PipelineTraceEntry &entry, bool initialize) {
	// an initialize task is bookkeeping that runs outside the pipeline's threads, usually on its query's client
	// thread: it is recorded only as initialize times, not as running work
	if (initialize) {
		entry.SetOnce(entry.initialize_start_ns, Now());
		auto initialize_worker = current_worker;
		if (initialize_worker.IsValid() && initialize_worker.GetIndex() < MAX_WORKERS) {
			entry.initialize_on_worker = true;
		}
		return;
	}
	entry.running_threads++;
	auto worker = current_worker;
	if (worker.IsValid() && worker.GetIndex() < MAX_WORKERS) {
		worker_pipelines[worker.GetIndex()] = &entry;
	} else {
		external_running++;
	}
}

void PipelineTracer::StageTaskFinished(PipelineTraceEntry &entry, int64_t start_ns, bool initialize) {
	if (initialize) {
		auto now = Now();
		entry.initialize_thread_ns += NumericCast<uint64_t>(MaxValue<int64_t>(now - start_ns, 0));
		entry.SetMax(entry.initialize_end_ns, now);
		return;
	}
	auto worker = current_worker;
	if (worker.IsValid() && worker.GetIndex() < MAX_WORKERS) {
		worker_pipelines[worker.GetIndex()] = nullptr;
	} else {
		external_running--;
	}
	entry.running_threads--;
	entry.finalize_thread_ns += NumericCast<uint64_t>(MaxValue<int64_t>(Now() - start_ns, 0));
	entry.finalize_tasks++;
}

void PipelineTracer::Reset() {
	if (hook) {
		hook->ClearTaskLog();
	}
	lock_guard<mutex> guard(lock);
	finished_queries.clear();
	recently_finished.clear();
	interval_rows.Clear();
	tick_rows.Clear();
	next_tick = 0;
	last_tick_ns = TRACE_NOT_SET;
}

static Value IdList(const vector<idx_t> &ids) {
	vector<Value> values;
	for (auto id : ids) {
		values.push_back(Value::UBIGINT(id));
	}
	return Value::LIST(LogicalType::UBIGINT, std::move(values));
}

static Value Timestamp(int64_t ns) {
	return ns == TRACE_NOT_SET ? Value(LogicalType::BIGINT) : Value::BIGINT(ns);
}

idx_t PipelineTracer::GetIntervalRowCount() {
	lock_guard<mutex> guard(lock);
	return interval_rows.Size();
}

vector<vector<Value>> PipelineTracer::GetIntervalRows(idx_t offset, idx_t count) {
	lock_guard<mutex> guard(lock);
	vector<vector<Value>> result;
	auto end = MinValue<idx_t>(offset + count, interval_rows.Size());
	for (idx_t i = offset; i < end; i++) {
		auto &row = interval_rows[i];
		result.push_back({Value::UBIGINT(row.tick), Value::BIGINT(row.t_ns), Value::UBIGINT(row.query_id),
		                  Value::UBIGINT(row.pipeline_id), Value(TraceStatusName(row.status)),
		                  Value::UBIGINT(row.threads), IdList(row.workers.ToList()), Value::UBIGINT(row.running),
		                  Value::UBIGINT(row.assigned), Value::UBIGINT(row.source_chunks),
		                  Value::UBIGINT(row.source_rows), Value::UBIGINT(row.sink_rows), Value::UBIGINT(row.thread_ns),
		                  Value::UBIGINT(row.finalize_thread_ns)});
	}
	return result;
}

idx_t PipelineTracer::GetTickRowCount() {
	lock_guard<mutex> guard(lock);
	return tick_rows.Size();
}

vector<vector<Value>> PipelineTracer::GetTickRows(idx_t offset, idx_t count) {
	lock_guard<mutex> guard(lock);
	vector<vector<Value>> result;
	auto end = MinValue<idx_t>(offset + count, tick_rows.Size());
	for (idx_t i = offset; i < end; i++) {
		auto &row = tick_rows[i];
		result.push_back({Value::UBIGINT(row.tick), Value::BIGINT(row.t_ns), Value::UBIGINT(row.busy_workers),
		                  Value::UBIGINT(row.busy_external), Value::UBIGINT(row.active_queries),
		                  Value::UBIGINT(row.active_pipelines), Value::UBIGINT(row.free_workers),
		                  Value::UBIGINT(row.untraced_tasks), Value::UBIGINT(row.busy_ns),
		                  Value::UBIGINT(row.attached_idle_ns), Value::UBIGINT(row.free_ns),
		                  Value::BIGINT(row.tick_work_ns)});
	}
	return result;
}

vector<vector<Value>> PipelineTracer::GetPipelineRows() {
	lock_guard<mutex> guard(lock);
	vector<vector<Value>> result;
	for (auto *queries : {&finished_queries, &active_queries}) {
		for (auto &query : *queries) {
			for (auto &entry_ptr : query->pipelines) {
				auto &entry = *entry_ptr;
				vector<Value> names;
				for (auto &name : entry.operator_names) {
					names.emplace_back(name);
				}
				auto task_count = entry.task_count.load();
				result.push_back({Value::UBIGINT(entry.query_id),
				                  Value::UBIGINT(entry.pipeline_id),
				                  IdList(entry.operator_ids),
				                  Value::LIST(LogicalType::VARCHAR, std::move(names)),
				                  IdList(entry.dependencies),
				                  Value::UBIGINT(entry.source_estimated_cardinality),
				                  task_count == TRACE_NOT_SET ? Value(LogicalType::BIGINT) : Value::BIGINT(task_count),
				                  Value(TraceStatusName(GetStatus(entry))),
				                  Timestamp(entry.scheduled_ns.load()),
				                  Timestamp(entry.first_task_ns.load()),
				                  Timestamp(entry.last_source_ns.load()),
				                  Timestamp(entry.execute_end_ns.load()),
				                  Timestamp(entry.finalize_start_ns.load()),
				                  Timestamp(entry.finalize_end_ns.load()),
				                  Value::UBIGINT(entry.source_chunks.load()),
				                  Value::UBIGINT(entry.source_rows.load()),
				                  Value::UBIGINT(entry.sink_rows.load()),
				                  Value::UBIGINT(entry.thread_ns.load()),
				                  Value::UBIGINT(entry.finalize_thread_ns.load()),
				                  Value::UBIGINT(entry.finalize_tasks.load()),
				                  Value::UBIGINT(entry.initialize_thread_ns.load()),
				                  Timestamp(entry.eligible_ns.load()),
				                  Value::UBIGINT(entry.task_wait_ns.load()),
				                  Value::UBIGINT(entry.tasks_started.load()),
				                  Timestamp(entry.complete_ns.load()),
				                  Timestamp(entry.initialize_start_ns.load()),
				                  Timestamp(entry.initialize_end_ns.load()),
				                  Value::BOOLEAN(entry.initialize_on_worker.load())});
			}
		}
	}
	return result;
}

vector<vector<Value>> PipelineTracer::GetQueryRows() {
	lock_guard<mutex> guard(lock);
	vector<vector<Value>> result;
	for (auto *queries : {&finished_queries, &active_queries}) {
		for (auto &query : *queries) {
			result.push_back({Value::UBIGINT(query->query_id), Value(query->tag), Value(query->query),
			                  Timestamp(query->submitted_ns), Timestamp(query->finished_ns),
			                  Value::BOOLEAN(query->error), Value::UBIGINT(query->pipelines.size())});
		}
	}
	return result;
}

} // namespace duckdb
