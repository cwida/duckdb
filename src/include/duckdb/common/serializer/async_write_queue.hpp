//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/async_write_queue.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/enums/file_write_mode.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/async_memory_governor.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
class TaskExecutor;

//! Compile-time policy used by the async write layers.
struct AsyncWriteConfig {
	//! Capacity of the staging buffer used for small transient stream writes.
	static constexpr idx_t COPIED_BUFFER_CAPACITY = 4096;
	//! Maximum bytes a single low-level async task should drain before yielding scheduler capacity.
	static constexpr idx_t TASK_BYTE_BUDGET = 4ULL * 1024ULL * 1024ULL;
	//! Local file systems are cheap to call, so only coalesce up to the buffered writer page size.
	static constexpr idx_t LOCAL_COALESCE_THRESHOLD = 4096;
	//! Remote file systems benefit from fewer round trips, so coalesce contiguous small buffers more aggressively.
	static constexpr idx_t REMOTE_COALESCE_THRESHOLD = 8ULL * 1024ULL * 1024ULL;
	//! Maximum bytes a single managed stream request should submit before yielding scheduler capacity.
	static constexpr idx_t DRAIN_TASK_BYTE_BUDGET = 16ULL * 1024ULL * 1024ULL;
};

//! Owned payload that can be handed to an async write queue.
class AsyncWritePayload {
public:
	virtual ~AsyncWritePayload() = default;

	//! Pointer to the bytes to write. The buffer must remain valid for the lifetime of this object.
	virtual data_ptr_t Ptr() = 0;
	//! Number of bytes exposed by Ptr().
	virtual idx_t Size() const = 0;
};

//! Compatibility name for existing stream-oriented callers.
using AsyncWriteBuffer = AsyncWritePayload;

//! Completion callback for one physical async write request. The error is set when the write failed.
using AsyncWriteCompletionCallback = std::function<void(idx_t offset, idx_t size, optional_ptr<const ErrorData> error)>;

//! One positional physical write request.
class AsyncWriteRequest {
public:
	AsyncWriteRequest() = default;
	AsyncWriteRequest(unique_ptr<AsyncWritePayload> payload, idx_t offset,
	                  AsyncWriteCompletionCallback completion = nullptr);

	idx_t Size() const;

	unique_ptr<AsyncWritePayload> payload;
	idx_t offset = 0;
	AsyncWriteCompletionCallback completion;
};

//! Positional physical write target used by the low-level AsyncWriteQueue.
class AsyncWriteTarget {
public:
	virtual ~AsyncWriteTarget() = default;

	//! Write a specific byte range using the target's explicit-offset write path.
	virtual void Write(data_ptr_t buffer, idx_t size, idx_t offset) = 0;
};

//! Minimal positional async write scheduler.
//! Requests are independent positional writes; stream ordering, coalescing, and memory policy live in wrappers.
class AsyncWriteQueue {
	friend class AsyncWriteQueueTask;
	friend class AsyncWriteQueueTaskGuard;

public:
	DUCKDB_API AsyncWriteQueue(ClientContext &client_context, AsyncWriteTarget &target);
	DUCKDB_API ~AsyncWriteQueue();

	AsyncWriteQueue(const AsyncWriteQueue &) = delete;
	AsyncWriteQueue &operator=(const AsyncWriteQueue &) = delete;

public:
	//! Return whether writes are drained by async scheduler tasks. If false, Submit writes synchronously.
	DUCKDB_API bool IsAsync() const;
	//! Return whether the async task executor has captured an error.
	DUCKDB_API bool HasError();
	//! Submit one owned positional request to the configured sync/async write path.
	DUCKDB_API void Submit(AsyncWriteRequest request);
	//! Return queued/in-flight bytes that have not reached the target yet.
	DUCKDB_API idx_t PendingBytes();
	//! Execute one queued task owned by this queue, or yield if the tasks are already running.
	DUCKDB_API void WorkOnPendingTask();
	//! Wait until all submitted writes have reached the target.
	DUCKDB_API void Flush();
	//! Wait for all writes and close the queue.
	DUCKDB_API void Close();
	//! Stop scheduling writes, discard pending requests, and wait for scheduled requests.
	DUCKDB_API void AbortWrites();
	//! Surface an error thrown by an async drain task.
	DUCKDB_API void RethrowTaskError();

private:
	struct PendingRequest {
		PendingRequest() = default;
		PendingRequest(AsyncWriteRequest request, idx_t size) noexcept;

		idx_t Size() const;

		AsyncWriteRequest request;
		idx_t size;
	};

private:
	//! Schedule pending requests until max_active_tasks is reached.
	void ScheduleTasksInternal(bool force = false);
	//! Return the byte budget for one task.
	idx_t TaskByteBudget() const;
	//! Return how many bytes one task should reserve after skipping already scheduled bytes.
	idx_t SelectPendingRequestBytes(idx_t skip_bytes) const;
	//! Move one reserved prefix of pending requests into a write task. Caller owns a scheduled task slot.
	idx_t TakeRequests(deque<PendingRequest> &requests);
	//! Release one scheduled/running task slot and its in-flight byte accounting.
	void FinishTask(idx_t task_size);
	//! Release a task slot for a scheduled task that never entered the queue because another task failed.
	void CancelScheduledTask();
	//! Release multiple reserved task slots that were never scheduled.
	void CancelScheduledTasks(idx_t task_count);

	//! Async task entry point that drains a bounded batch of positional requests.
	void DrainRequests();
	//! Write request bytes to the target and invoke its completion callback.
	void WriteRequest(AsyncWriteRequest request);
	//! Invoke a completion callback outside the queue lock.
	void CompleteRequest(AsyncWriteRequest &request, idx_t size, optional_ptr<const ErrorData> error);
	//! Write bytes to the target at the assigned physical offset.
	void WriteBuffer(data_ptr_t buffer, idx_t size, idx_t offset);
	//! Throw if a mutating API is used after Close().
	void VerifyOpen() const;
	//! Throw if the queue still owns registered or scheduled write work.
	void VerifyDrained() const;
	//! Fail and discard queued requests after an async write failure once all scheduled tasks have stopped.
	void CancelPendingRequestsAfterFailure(const ErrorData &error) noexcept;

private:
	ClientContext &client_context;
	AsyncWriteTarget &target;
	//! Maximum scheduled/running write tasks for this queue.
	idx_t max_active_tasks = 1;
	//! Maximum bytes a single async task should drain.
	idx_t task_byte_budget = AsyncWriteConfig::TASK_BYTE_BUDGET;

	//! Protects state shared between the submitting thread and async write tasks.
	mutex lock;
	//! Positional requests waiting for an async task.
	deque<PendingRequest> pending_requests;
	//! Bytes queued in pending_requests that have not been taken by a task yet.
	idx_t pending_bytes = 0;
	//! Bytes owned by write tasks that have not reached the target yet.
	idx_t in_flight_bytes = 0;
	//! Bytes in pending_requests already reserved by scheduled-but-not-started tasks.
	idx_t scheduled_pending_bytes = 0;
	//! Scheduled or running write tasks for this queue.
	idx_t active_tasks = 0;
	//! Scheduled write tasks that have not yet claimed a request.
	idx_t pending_tasks = 0;
	//! Per-task byte reservations for scheduled tasks that have not yet claimed their requests.
	deque<idx_t> pending_task_bytes;
	//! Set after Close() has drained the queue. Further submissions are rejected.
	bool closed = false;

	//! Async task executor. If absent, writes are performed synchronously on submission.
	//! Keep this after task-accounting fields so queued task destructors can still release slots.
	unique_ptr<TaskExecutor> executor;
};

//! Stream target used by ManagedAsyncWriteStreamQueue. Sequential fallback is handled here, not in AsyncWriteQueue.
class ManagedAsyncWriteStreamTarget {
public:
	virtual ~ManagedAsyncWriteStreamTarget() = default;

	//! Return the target's write ordering contract.
	virtual FileWriteMode GetWriteMode() = 0;
	//! Whether this target is a local file-like target. Remote targets use larger coalesced writes.
	virtual bool IsLocalFile() = 0;
	//! Write a specific byte range using the target's explicit-offset write path.
	virtual void Write(data_ptr_t buffer, idx_t size, idx_t offset) = 0;
	//! Write bytes using the target's sequential write path.
	virtual void Write(data_ptr_t buffer, idx_t size) = 0;
};

//! Managed positional write queue built on top of AsyncWriteQueue.
//! Requests may target independent offsets; stream ordering and coalescing live in ManagedAsyncWriteStreamQueue.
class ManagedAsyncWriteQueue : private AsyncWriteTarget {
	friend class ManagedAsyncWriteStreamQueue;
	friend class ManagedAsyncWriteQueueTest;

public:
	//! Whether registering a payload may schedule an async drain request immediately.
	enum class ScheduleMode : uint8_t { ALLOW, DEFER };
	//! Whether to force a TemporaryMemoryState growth check instead of relying on coarse growth.
	enum class MemoryUpdateMode : uint8_t { COARSE, FORCE };
	//! Whether to schedule only enough request capacity for normal overlap, or force all pending bytes to drain.
	enum class SchedulePolicy : uint8_t { THRESHOLD, FORCE };

public:
	DUCKDB_API ManagedAsyncWriteQueue(ClientContext &client_context, AsyncWriteTarget &target);
	DUCKDB_API ~ManagedAsyncWriteQueue() override;

	ManagedAsyncWriteQueue(const ManagedAsyncWriteQueue &) = delete;
	ManagedAsyncWriteQueue &operator=(const ManagedAsyncWriteQueue &) = delete;

public:
	//! Return whether writes are drained by async scheduler tasks. If false, RegisterWrite writes synchronously.
	DUCKDB_API bool IsAsync() const;
	//! Return whether the async task executor has captured an error.
	DUCKDB_API bool HasError();
	//! Add an owned positional payload to the configured sync/async write path.
	DUCKDB_API void RegisterWrite(unique_ptr<AsyncWritePayload> payload, idx_t offset,
	                              ScheduleMode schedule_mode = ScheduleMode::ALLOW);
	//! Add one positional request to the configured sync/async write path.
	DUCKDB_API void RegisterWrite(AsyncWriteRequest request, ScheduleMode schedule_mode = ScheduleMode::ALLOW);
	//! Schedule as many drain requests as the pending queue allows.
	DUCKDB_API void SchedulePendingWrites(SchedulePolicy policy = SchedulePolicy::THRESHOLD);
	//! Help drain async writes when pending bytes exceed the current memory budget.
	DUCKDB_API void ApplyBackpressure();
	//! Wait until all registered writes have reached the target.
	DUCKDB_API void WaitAll();
	//! Drain all writes, close the queue, and release the TemporaryMemoryState reservation.
	DUCKDB_API void Close();
	//! Stop scheduling writes, discard pending requests, and wait for submitted requests.
	DUCKDB_API void AbortWrites();
	//! Release the queue's TemporaryMemoryState reservation.
	DUCKDB_API void ReleaseMemoryReservation();
	//! Surface an error thrown by an async drain task.
	DUCKDB_API void RethrowTaskError();

private:
	enum class AccountedWriteAdoption : uint8_t { ACCEPTED, REJECTED };

	struct PendingWrite {
		PendingWrite(AsyncWriteRequest request, idx_t size) noexcept;

		idx_t Size() const;

		AsyncWriteRequest request;
		idx_t size;
	};

private:
	//! Try to adopt one positional request whose bytes are already tracked as external pending bytes.
	//! The request is consumed if and only if ACCEPTED is returned.
	AccountedWriteAdoption TryAdoptAccountedWrite(AsyncWriteRequest &request, ErrorData &error);
	//! Track bytes held by a wrapper before they become positional requests.
	void AddExternalPendingBytes(idx_t bytes, bool update_memory = true);
	//! Stop tracking wrapper-held bytes that will never become positional requests.
	void DiscardExternalPendingBytes(idx_t bytes) noexcept;
	//! Add one request to the managed queue. Caller may mark bytes already tracked as external.
	void RegisterWriteInternal(AsyncWriteRequest request, idx_t accounted_external_bytes, ScheduleMode schedule_mode);
	//! Schedule drain requests from already registered pending writes.
	void SchedulePendingWritesInternal(SchedulePolicy policy = SchedulePolicy::THRESHOLD);
	//! Grow the TemporaryMemoryState reservation coarsely; it is released only when the queue closes.
	void UpdateMemoryState(MemoryUpdateMode mode = MemoryUpdateMode::COARSE);

	//! Return the current async backlog budget after applying the fixed queue cap.
	idx_t BackpressureBudget();
	//! Effective byte budget for one managed drain request.
	idx_t DrainTaskByteBudget() const;
	//! Return queued/submitted/external bytes that have not reached the target yet. Caller must hold lock.
	idx_t TotalPendingBytes() const;
	//! Return how many physical bytes can be submitted to the low-level queue before refilling should pause.
	idx_t SubmittedByteWindow() const;

	//! Move one pending positional write into a physical async request.
	bool TakePendingWriteRequest(AsyncWriteRequest &request, SchedulePolicy policy);
	//! Prepare a callback that releases submitted-byte accounting before user callbacks run.
	AsyncWriteCompletionCallback CreateCompletionAccounting(const AsyncWriteCompletionCallback &user_completion);
	//! Release byte accounting for one submitted physical request.
	void CompleteSubmittedWrite(idx_t offset, idx_t size, optional_ptr<const ErrorData> error);

	//! Throw if a mutating API is used after Close().
	void VerifyOpen() const;
	//! Throw if the queue still owns registered or scheduled write work.
	void VerifyDrained() const;
	//! Fail and discard queued writes after an async write failure once all submitted writes have stopped.
	void CancelPendingWritesAfterFailure(const ErrorData &error) noexcept;

	//! Write bytes to the managed target at the assigned physical offset.
	void Write(data_ptr_t buffer, idx_t size, idx_t offset) override;

private:
	ClientContext &client_context;
	AsyncWriteTarget &target;

	//! Low-level positional request scheduler.
	unique_ptr<AsyncWriteQueue> write_queue;
	//! Shared TemporaryMemoryManager reservation governor bounding queued async write data.
	ManagedAsyncMemoryGovernor memory_governor;
	//! Maximum number of submitted/running drain requests for this queue.
	idx_t max_active_drain_tasks = 1;
	//! Maximum bytes one managed async request should submit before yielding scheduler capacity.
	idx_t drain_task_byte_budget = AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET;

	//! Protects state shared between registering threads and async completion callbacks.
	mutex lock;
	//! Positional payloads queued for submission to AsyncWriteQueue.
	deque<PendingWrite> pending_writes;
	//! Bytes queued in pending_writes that have not been submitted to AsyncWriteQueue yet.
	idx_t pending_bytes = 0;
	//! Bytes tracked by a wrapper before they become positional requests.
	idx_t external_pending_bytes = 0;
	//! Bytes submitted to AsyncWriteQueue that have not completed yet.
	idx_t submitted_bytes = 0;
	//! Submitted physical requests that have not completed yet.
	idx_t submitted_requests = 0;
	//! Set after Close() has drained the queue. Further write registration is rejected.
	bool closed = false;
};

//! Managed stream-oriented write queue built on top of ManagedAsyncWriteQueue.
//! V1 is a contiguous logical write queue: each RegisterWrite offset must match the next expected offset.
//! Callers are responsible for assigning offsets and externally serializing RegisterWrite calls.
class ManagedAsyncWriteStreamQueue : private AsyncWriteTarget {
	friend class ManagedAsyncWriteQueueTest;

public:
	//! Whether registering a payload may schedule an async drain request immediately.
	enum class ScheduleMode : uint8_t { ALLOW, DEFER };
	//! Whether to schedule only enough request capacity for normal overlap, or force all pending bytes to drain.
	enum class SchedulePolicy : uint8_t { THRESHOLD, FORCE };
	//! Whether waiting for scheduled writes should preserve an open registration batch.
	enum class BatchDrainMode : uint8_t { PRESERVE_BATCH, FORCE_CLOSE_BATCH };

public:
	DUCKDB_API ManagedAsyncWriteStreamQueue(ClientContext &client_context, ManagedAsyncWriteStreamTarget &target);
	DUCKDB_API ~ManagedAsyncWriteStreamQueue() override;

	ManagedAsyncWriteStreamQueue(const ManagedAsyncWriteStreamQueue &) = delete;
	ManagedAsyncWriteStreamQueue &operator=(const ManagedAsyncWriteStreamQueue &) = delete;

public:
	//! Return whether writes are drained by async scheduler tasks. If false, RegisterWrite writes synchronously.
	DUCKDB_API bool IsAsync() const;
	//! Return whether the async task executor has captured an error.
	DUCKDB_API bool HasError();
	//! Add an owned payload at the next contiguous logical offset to the configured sync/async write path.
	DUCKDB_API void RegisterWrite(unique_ptr<AsyncWritePayload> payload, idx_t offset,
	                              ScheduleMode schedule_mode = ScheduleMode::ALLOW);
	//! Enter a registration batch, delaying async draining until the batch is left.
	DUCKDB_API void BeginBatch();
	//! Leave a registration batch without scheduling, blocking, or throwing.
	DUCKDB_API void LeaveBatch() noexcept;
	//! Return whether a registration batch is currently open.
	DUCKDB_API bool HasOpenBatch();
	//! Schedule as many drain requests as the pending queue allows.
	DUCKDB_API void SchedulePendingWrites(SchedulePolicy policy = SchedulePolicy::THRESHOLD);
	//! Help drain async writes when pending bytes exceed the current memory budget. No-op while a batch is open.
	DUCKDB_API void ApplyBackpressure();
	//! Wait for scheduled writes, optionally restoring an active registration batch afterwards.
	DUCKDB_API void WaitAll(BatchDrainMode batch_drain_mode = BatchDrainMode::PRESERVE_BATCH);
	//! Drain all writes, close any open registration batch, and release the TemporaryMemoryState reservation.
	DUCKDB_API void Close();
	//! Stop scheduling writes, discard pending writes, and wait for submitted writes.
	DUCKDB_API void AbortWrites();
	//! Reset the next expected contiguous offset after all registered writes have drained.
	DUCKDB_API void ResetNextOffset(idx_t offset);
	//! Release the queue's TemporaryMemoryState reservation.
	DUCKDB_API void ReleaseMemoryReservation();
	//! Surface an error thrown by an async drain task.
	DUCKDB_API void RethrowTaskError();

private:
	struct PendingWrite {
		PendingWrite(unique_ptr<AsyncWritePayload> payload, idx_t offset);

		idx_t Size() const;

		unique_ptr<AsyncWritePayload> payload;
		idx_t offset;
	};

	class CoalescedWritePayload;
	class MaterializedWritePayload;

private:
	//! Schedule drain requests from already registered pending writes.
	void SchedulePendingWritesInternal(SchedulePolicy policy = SchedulePolicy::THRESHOLD);

	//! Effective byte budget for one managed drain request, never smaller than the coalescing threshold.
	idx_t DrainTaskByteBudget() const;
	//! Return queued/submitted bytes that have not reached the target yet. Caller must hold lock.
	idx_t TotalPendingBytes() const;
	//! Select the pending write range one primitive task would claim. Caller must hold lock.
	idx_t SelectPendingWriteEnd(idx_t start, idx_t &selected_bytes) const;
	//! Select the pending write range for the next physical write request. Caller must hold lock.
	idx_t SelectPhysicalWriteEnd(idx_t start, idx_t &selected_bytes) const;
	//! Return how many physical bytes can be submitted to the low-level queue before refilling should pause.
	idx_t SubmittedByteWindow() const;

	//! Move one byte-budgeted prefix of pending writes into a physical async request.
	bool TakePendingWriteRequest(AsyncWriteRequest &request, SchedulePolicy policy);
	//! Convert one or more contiguous pending writes into a lazily materialized payload.
	unique_ptr<AsyncWritePayload> CreatePayload(deque<PendingWrite> writes, idx_t size);
	//! Materialize one pending concurrent-sequential physical request before publishing it to the lower queue.
	unique_ptr<AsyncWritePayload> CreateMaterializedPayload(idx_t end, idx_t size);
	//! Release byte accounting for one submitted physical request.
	void CompleteSubmittedWrite(idx_t offset, idx_t size, optional_ptr<const ErrorData> error);
	//! Latch one local scheduling failure, stop publication, and discard unsubmitted stream work.
	void FailLocalScheduling(ErrorData error, idx_t unaccepted_size = 0);
	//! Return the locally latched scheduling failure, if any. Caller must hold lock.
	shared_ptr<const ErrorData> GetLocalError() const DUCKDB_REQUIRES(lock);

	//! Validate a new registration against the contiguous offset contract.
	idx_t ValidateRegistrationOffset(idx_t offset, idx_t write_size) const;
	//! Throw if a mutating API is used after Close().
	void VerifyOpen() const;
	//! Validate a pending write before coalescing it with its predecessor.
	void VerifyContiguousWrite(const PendingWrite &write, idx_t expected_offset) const;
	//! Return offset + write_size, throwing if it overflows idx_t.
	idx_t NextWriteOffset(idx_t offset, idx_t write_size) const;
	//! Throw if the queue still owns registered or scheduled write work.
	void VerifyDrained() const;
	//! Discard queued writes after an async write failure once all submitted writes have stopped.
	void CancelPendingWritesAfterFailure() noexcept;

	//! Write bytes to the managed target. SEQUENTIAL mode preserves queue order and ignores the logical offset here.
	void Write(data_ptr_t buffer, idx_t size, idx_t offset) override;

private:
	ClientContext &client_context;
	ManagedAsyncWriteStreamTarget &target;

	//! Managed queue that owns TMM reservation, backpressure, and task scheduling for physical write requests.
	unique_ptr<ManagedAsyncWriteQueue> write_queue;
	//! The write ordering contract exposed by the target.
	FileWriteMode write_mode = FileWriteMode::SEQUENTIAL;
	//! Maximum number of submitted/running drain requests for this queue.
	idx_t max_active_drain_tasks = 1;
	//! Size below which adjacent writes are coalesced before reaching the target.
	idx_t coalesce_threshold = 0;
	//! Minimum queued bytes before threshold scheduling starts the first async task.
	idx_t first_task_schedule_threshold = 0;
	//! Maximum bytes one stream request should hand to the positional managed queue.
	idx_t drain_task_byte_budget = 0;
	//! Stop each local coalesced write at coalesce_threshold.
	bool limit_coalesced_write_size = false;

	//! Protects state shared between the registering thread and async completion callbacks.
	mutable annotated_mutex lock;
	//! Serializes selection, materialization, accounting transfer, and lower-queue publication.
	mutex submission_lock;
	//! Pending payloads in registration order with pre-assigned logical offsets.
	deque<PendingWrite> pending_writes;
	//! Bytes queued in pending_writes that have not been submitted to AsyncWriteQueue yet.
	idx_t pending_bytes = 0;
	//! Bytes submitted to AsyncWriteQueue that have not completed yet.
	idx_t submitted_bytes = 0;
	//! Submitted physical requests that have not completed yet.
	idx_t submitted_requests = 0;
	//! Nested batch depth. While non-zero, async draining and backpressure are delayed.
	idx_t batch_depth = 0;
	//! Whether completion-driven refills should ignore the normal first-task threshold.
	bool force_completion_refill = false;
	//! Next logical offset expected by RegisterWrite. Enforces v1 contiguous-registration semantics.
	idx_t next_registration_offset = 0;
	//! Set after Close() has drained the queue. Further write registration is rejected.
	bool closed = false;
	//! First failure that occurred before a request entered the lower async queue.
	shared_ptr<const ErrorData> local_error DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
