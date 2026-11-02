#include "duckdb/common/serializer/async_task_queue.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <exception>

namespace duckdb {

static ErrorData TaskErrorDataFromExceptionPtr(const std::exception_ptr &error_ptr) {
	try {
		std::rethrow_exception(error_ptr);
	} catch (const std::exception &ex) {
		return ErrorData(ex);
	} catch (...) { // LCOV_EXCL_START
		return ErrorData("Unknown exception during async task");
	} // LCOV_EXCL_STOP
}

AsyncTaskRequest::AsyncTaskRequest(unique_ptr<AsyncTask> task_p, idx_t size_p, AsyncTaskCompletionCallback completion_p)
    : task(std::move(task_p)), size(size_p), completion(std::move(completion_p)) {
}

idx_t AsyncTaskRequest::Size() const {
	return size;
}

//===--------------------------------------------------------------------===//
// AsyncTaskQueue
//===--------------------------------------------------------------------===//
class AsyncTaskQueueTaskGuard {
public:
	explicit AsyncTaskQueueTaskGuard(AsyncTaskQueue &queue_p) : queue(queue_p) {
	}

	~AsyncTaskQueueTaskGuard() {
		Finish();
	}

	void SetRequestSize(idx_t request_size_p) {
		D_ASSERT(!finished);
		request_size = request_size_p;
	}

	void Finish() {
		if (!finished) {
			queue.FinishTask(request_size);
			finished = true;
		}
	}

private:
	AsyncTaskQueue &queue;
	idx_t request_size = 0;
	bool finished = false;
};

class AsyncTaskQueueTask : public BaseExecutorTask {
public:
	AsyncTaskQueueTask(AsyncTaskQueue &queue_p, TaskExecutor &executor) : BaseExecutorTask(executor), queue(queue_p) {
	}

	~AsyncTaskQueueTask() override {
		if (!started) {
			queue.CancelScheduledTask();
		}
	}

	void ExecuteTask() override {
		started = true;
		queue.DrainRequest();
	}

private:
	AsyncTaskQueue &queue;
	bool started = false;
};

AsyncTaskQueue::AsyncTaskQueue(ClientContext &client_context_p, idx_t max_active_tasks_p)
    : client_context(client_context_p) {
	auto &scheduler = TaskScheduler::GetScheduler(client_context);
	auto async_threads = NumericCast<idx_t>(scheduler.NumberOfAsyncThreads());
	max_active_tasks = max_active_tasks_p > 0 ? max_active_tasks_p : MaxValue<idx_t>(async_threads, 1);
	if (async_threads == 0) {
		return;
	}
	executor = make_uniq<TaskExecutor>(client_context, TaskSchedulerType::ASYNC);
}

AsyncTaskQueue::~AsyncTaskQueue() {
	lock_guard<mutex> guard(lock);
#ifdef D_ASSERT_IS_ENABLED
	auto drained = pending_requests.empty() && pending_bytes == 0 && in_flight_bytes == 0 && active_tasks == 0 &&
	               pending_tasks == 0;
	D_ASSERT(closed || drained);
	D_ASSERT(!closed || drained);
#endif
}

bool AsyncTaskQueue::IsAsync() const {
	return executor != nullptr;
}

bool AsyncTaskQueue::HasError() {
	return executor && executor->HasError();
}

void AsyncTaskQueue::Submit(AsyncTaskRequest request) {
	if (!request.task) {
		return;
	}
	auto request_size = request.Size();
	if (executor && executor->HasError()) {
		auto error = executor->GetError();
		request.task.reset();
		CompleteRequest(request, request_size, error);
		error.Throw();
	}
	if (!executor) {
		VerifyOpen();
		ExecuteRequest(std::move(request));
		return;
	}

	{
		lock_guard<mutex> guard(lock);
		VerifyOpen();
		pending_requests.push_back(std::move(request));
		pending_bytes += request_size;
	}
	ScheduleTasksInternal();
}

idx_t AsyncTaskQueue::PendingBytes() {
	lock_guard<mutex> guard(lock);
	return pending_bytes + in_flight_bytes;
}

void AsyncTaskQueue::ScheduleTasksInternal() {
	if (!executor) {
		return;
	}
	idx_t schedule_count = 0;
	{
		lock_guard<mutex> guard(lock);
		VerifyOpen();
		// One drain task per still-unclaimed pending request, capped at the concurrency limit.
		while (pending_tasks + schedule_count < pending_requests.size() &&
		       active_tasks + schedule_count < max_active_tasks) {
			schedule_count++;
		}
		active_tasks += schedule_count;
		pending_tasks += schedule_count;
	}
	for (idx_t task_idx = 0; task_idx < schedule_count; task_idx++) {
		unique_ptr<AsyncTaskQueueTask> task;
		try {
			task = make_uniq<AsyncTaskQueueTask>(*this, *executor);
		} catch (...) {
			CancelScheduledTasks(schedule_count - task_idx);
			throw;
		}
		try {
			executor->ScheduleTask(std::move(task));
		} catch (...) {
			// The task destructor releases this task's slot. Release the slots for tasks not yet created.
			CancelScheduledTasks(schedule_count - task_idx - 1);
			throw;
		}
	}
}

void AsyncTaskQueue::FinishTask(idx_t task_size) {
	lock_guard<mutex> guard(lock);
	D_ASSERT(active_tasks > 0);
	active_tasks--;
	D_ASSERT(in_flight_bytes >= task_size);
	in_flight_bytes -= task_size;
}

void AsyncTaskQueue::CancelScheduledTask() {
	CancelScheduledTasks(1);
}

void AsyncTaskQueue::CancelScheduledTasks(idx_t task_count) {
	if (task_count == 0) {
		return;
	}
	lock_guard<mutex> guard(lock);
	D_ASSERT(active_tasks >= task_count);
	D_ASSERT(pending_tasks >= task_count);
	active_tasks -= task_count;
	pending_tasks -= task_count;
}

void AsyncTaskQueue::DrainRequest() {
	AsyncTaskQueueTaskGuard guard(*this);
	AsyncTaskRequest request;
	bool has_request = false;
	{
		lock_guard<mutex> task_guard(lock);
		D_ASSERT(active_tasks > 0);
		D_ASSERT(pending_tasks > 0);
		pending_tasks--;
		if (!pending_requests.empty()) {
			request = std::move(pending_requests.front());
			pending_requests.pop_front();
			D_ASSERT(pending_bytes >= request.size);
			pending_bytes -= request.size;
			in_flight_bytes += request.size;
			has_request = true;
		}
	}
	if (!has_request) {
		guard.Finish();
		return;
	}
	guard.SetRequestSize(request.size);
	ExecuteRequest(std::move(request));
	guard.Finish();
	ScheduleTasksInternal();
}

void AsyncTaskQueue::CompleteRequest(AsyncTaskRequest &request, idx_t size, optional_ptr<const ErrorData> error) {
	if (request.completion) {
		request.completion(size, error);
	}
}

void AsyncTaskQueue::ExecuteRequest(AsyncTaskRequest request) {
	auto request_size = request.Size();
	ErrorData task_error;
	bool has_error = false;
	try {
		request.task->Execute();
	} catch (const std::exception &ex) {
		task_error = ErrorData(ex);
		has_error = true;
	} catch (...) { // LCOV_EXCL_START
		task_error = ErrorData("Unknown exception during async task");
		has_error = true;
	} // LCOV_EXCL_STOP

	request.task.reset();
	if (has_error) {
		CompleteRequest(request, request_size, task_error);
		task_error.Throw();
	}
	CompleteRequest(request, request_size, nullptr);
}

void AsyncTaskQueue::RethrowTaskError() {
	if (executor && executor->HasError()) {
		executor->ThrowError();
	}
}

void AsyncTaskQueue::WorkOnPendingTask() {
	if (!executor) {
		VerifyOpen();
		return;
	}
	shared_ptr<Task> task;
	if (!executor->GetTask(task)) {
		TaskScheduler::YieldThread();
		return;
	}
#ifdef D_ASSERT_IS_ENABLED
	auto result = task->Execute(TaskExecutionMode::PROCESS_ALL);
	D_ASSERT(result != TaskExecutionResult::TASK_BLOCKED);
#else
	task->Execute(TaskExecutionMode::PROCESS_ALL);
#endif
	task.reset();
}

void AsyncTaskQueue::Flush() {
	bool already_closed;
	{
		lock_guard<mutex> guard(lock);
		already_closed = closed;
	}
	if (already_closed) {
		RethrowTaskError();
		return;
	}
	if (!executor) {
		RethrowTaskError();
		return;
	}

	{
		// join before leaving this scope, whether the scheduling succeeds or throws
		TaskExecutor::JoinGuard join(*executor);
		ScheduleTasksInternal();
	}
	RethrowTaskError();
}

void AsyncTaskQueue::VerifyDrained() const {
	if (!pending_requests.empty() || pending_bytes != 0 || in_flight_bytes != 0 || active_tasks != 0 ||
	    pending_tasks != 0) {
		throw InternalException("AsyncTaskQueue still owns submitted tasks");
	}
}

void AsyncTaskQueue::CancelPendingRequestsAfterFailure(const ErrorData &error) noexcept {
	deque<AsyncTaskRequest> requests;
	{
		lock_guard<mutex> guard(lock);
		D_ASSERT(active_tasks == 0);
		D_ASSERT(pending_tasks == 0);
		D_ASSERT(in_flight_bytes == 0);
		if (active_tasks != 0 || pending_tasks != 0 || in_flight_bytes != 0) {
			return;
		}

		requests = std::move(pending_requests);
		pending_bytes = 0;
		closed = true;
	}

	for (auto &request : requests) {
		auto request_size = request.Size();
		request.task.reset();
		try {
			CompleteRequest(request, request_size, error);
		} catch (...) {
		}
	}
}

void AsyncTaskQueue::Close() {
	bool already_closed;
	{
		lock_guard<mutex> guard(lock);
		already_closed = closed;
	}
	if (already_closed) {
		RethrowTaskError();
		return;
	}

	try {
		Flush();
	} catch (...) {
		auto error = std::current_exception();
		auto error_data = TaskErrorDataFromExceptionPtr(error);
		CancelPendingRequestsAfterFailure(error_data);
		std::rethrow_exception(error);
	}

	lock_guard<mutex> guard(lock);
	VerifyDrained();
	closed = true;
}

void AsyncTaskQueue::VerifyOpen() const {
	if (closed) {
		throw InternalException("Cannot use closed AsyncTaskQueue");
	}
}

//===--------------------------------------------------------------------===//
// ManagedAsyncTaskQueue
//===--------------------------------------------------------------------===//
ManagedAsyncTaskQueue::ManagedAsyncTaskQueue(ClientContext &client_context_p, idx_t max_active_tasks)
    : client_context(client_context_p), memory_governor(client_context_p) {
	auto &scheduler = TaskScheduler::GetScheduler(client_context);
	auto async_threads = NumericCast<idx_t>(scheduler.NumberOfAsyncThreads());
	max_active_drain_tasks = max_active_tasks > 0 ? max_active_tasks : MaxValue<idx_t>(async_threads, 1);
	task_queue = make_uniq<AsyncTaskQueue>(client_context, max_active_drain_tasks);
}

ManagedAsyncTaskQueue::~ManagedAsyncTaskQueue() {
	lock_guard<mutex> guard(lock);
#ifdef D_ASSERT_IS_ENABLED
	auto drained = pending_requests.empty() && pending_bytes == 0 && submitted_bytes == 0 && submitted_requests == 0;
	D_ASSERT(closed || drained);
	D_ASSERT(!closed || drained);
#endif
}

bool ManagedAsyncTaskQueue::IsAsync() const {
	return task_queue->IsAsync();
}

bool ManagedAsyncTaskQueue::HasError() {
	return task_queue->HasError();
}

void ManagedAsyncTaskQueue::Register(unique_ptr<AsyncTask> task, idx_t byte_size) {
	if (!task) {
		return;
	}
	RethrowTaskError();

	if (!task_queue->IsAsync()) {
		VerifyOpen();
		task_queue->Submit(AsyncTaskRequest(std::move(task), byte_size));
		return;
	}

	AsyncTaskRequest request(std::move(task), byte_size);
	{
		lock_guard<mutex> guard(lock);
		VerifyOpen();
		pending_requests.push_back(std::move(request));
		pending_bytes += byte_size;
	}
	UpdateMemoryState();
	SchedulePendingTasks();
}

void ManagedAsyncTaskQueue::SchedulePendingTasks(SchedulePolicy policy) {
	if (!task_queue->IsAsync()) {
		return;
	}
	while (true) {
		AsyncTaskRequest request;
		if (!TakePendingTaskRequest(request, policy)) {
			return;
		}
		task_queue->Submit(std::move(request));
	}
}

void ManagedAsyncTaskQueue::UpdateMemoryState() {
	if (!memory_governor.IsActive()) {
		return;
	}
	idx_t current_pending_bytes;
	{
		lock_guard<mutex> guard(lock);
		current_pending_bytes = TotalPendingBytes();
	}
	memory_governor.UpdateReservation(current_pending_bytes);
}

idx_t ManagedAsyncTaskQueue::TotalPendingBytes() const {
	return pending_bytes + submitted_bytes;
}

bool ManagedAsyncTaskQueue::TakePendingTaskRequest(AsyncTaskRequest &request, SchedulePolicy policy) {
	lock_guard<mutex> guard(lock);
	if (pending_requests.empty()) {
		return false;
	}
	// Keep at most max_active_drain_tasks submitted so the low-level queue's backlog stays bounded.
	if (policy == SchedulePolicy::THRESHOLD && submitted_requests >= max_active_drain_tasks) {
		return false;
	}

	auto request_size = pending_requests.front().Size();
	request = std::move(pending_requests.front());
	pending_requests.pop_front();
	D_ASSERT(pending_bytes >= request_size);
	pending_bytes -= request_size;
	submitted_bytes += request_size;
	submitted_requests++;
	// Attach accounting only on submission, so cancelled pending tasks never carry it.
	AddCompletionAccounting(request);
	return true;
}

void ManagedAsyncTaskQueue::AddCompletionAccounting(AsyncTaskRequest &request) {
	request.completion = [this](idx_t size, optional_ptr<const ErrorData> error) {
		CompleteSubmittedTask(size, error);
	};
}

void ManagedAsyncTaskQueue::CompleteSubmittedTask(idx_t size, optional_ptr<const ErrorData> error) {
	bool refill = false;
	{
		lock_guard<mutex> guard(lock);
		D_ASSERT(submitted_requests > 0);
		submitted_requests--;
		D_ASSERT(submitted_bytes >= size);
		submitted_bytes -= size;
		refill = !error && !closed && !pending_requests.empty();
	}
	if (refill) {
		SchedulePendingTasks();
	}
}

void ManagedAsyncTaskQueue::ApplyBackpressure() {
	if (!task_queue->IsAsync()) {
		VerifyOpen();
		return;
	}
	RethrowTaskError();
	UpdateMemoryState();
	SchedulePendingTasks();
	while (true) {
		idx_t current_pending_bytes;
		{
			lock_guard<mutex> guard(lock);
			current_pending_bytes = TotalPendingBytes();
		}
		if (current_pending_bytes <= memory_governor.BackpressureBudget()) {
			return;
		}
		SchedulePendingTasks(SchedulePolicy::FORCE);
		task_queue->WorkOnPendingTask();
		RethrowTaskError();
	}
}

void ManagedAsyncTaskQueue::WaitAll() {
	bool already_closed;
	{
		lock_guard<mutex> guard(lock);
		already_closed = closed;
	}
	if (already_closed) {
		RethrowTaskError();
		return;
	}
	if (!task_queue->IsAsync()) {
		RethrowTaskError();
		return;
	}

	try {
		UpdateMemoryState();
		while (true) {
			if (!task_queue->HasError()) {
				SchedulePendingTasks(SchedulePolicy::FORCE);
			}
			task_queue->Flush();
			lock_guard<mutex> guard(lock);
			if (pending_requests.empty() && pending_bytes == 0 && submitted_bytes == 0 && submitted_requests == 0) {
				break;
			}
		}
	} catch (...) {
		try {
			task_queue->Flush();
		} catch (...) {
		}
		throw;
	}

	RethrowTaskError();
}

void ManagedAsyncTaskQueue::VerifyDrained() const {
	if (!pending_requests.empty() || pending_bytes != 0 || submitted_bytes != 0 || submitted_requests != 0) {
		throw InternalException("ManagedAsyncTaskQueue still owns registered tasks");
	}
}

void ManagedAsyncTaskQueue::CancelPendingTasksAfterFailure(const ErrorData &error) noexcept {
	deque<AsyncTaskRequest> tasks;
	{
		lock_guard<mutex> guard(lock);
		D_ASSERT(submitted_requests == 0);
		D_ASSERT(submitted_bytes == 0);
		if (submitted_requests != 0 || submitted_bytes != 0) {
			return;
		}

		tasks = std::move(pending_requests);
		pending_bytes = 0;
		closed = true;
	}

	// Pending tasks were never submitted, so they carry no accounting; only a user completion (if any) fires.
	for (auto &request : tasks) {
		auto request_size = request.Size();
		request.task.reset();
		if (request.completion) {
			try {
				request.completion(request_size, error);
			} catch (...) {
			}
		}
	}
}

void ManagedAsyncTaskQueue::Close() {
	bool already_closed;
	{
		lock_guard<mutex> guard(lock);
		already_closed = closed;
	}
	if (already_closed) {
		RethrowTaskError();
		return;
	}

	try {
		WaitAll();
		task_queue->Close();
		memory_governor.Release();
	} catch (...) {
		auto error = std::current_exception();
		try {
			task_queue->Close();
		} catch (...) {
		}
		auto error_data = TaskErrorDataFromExceptionPtr(error);
		CancelPendingTasksAfterFailure(error_data);
		try {
			memory_governor.Release();
		} catch (...) {
		}
		std::rethrow_exception(error);
	}

	lock_guard<mutex> guard(lock);
	VerifyDrained();
	closed = true;
}

void ManagedAsyncTaskQueue::RethrowTaskError() {
	task_queue->RethrowTaskError();
}

void ManagedAsyncTaskQueue::VerifyOpen() const {
	if (closed) {
		throw InternalException("Cannot use closed ManagedAsyncTaskQueue");
	}
}

} // namespace duckdb
