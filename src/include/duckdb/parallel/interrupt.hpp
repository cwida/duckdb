//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/interrupt.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/parallel/task.hpp"

#include <condition_variable>

namespace duckdb {

//! InterruptMode specifies how operators should block/unblock, note that this will happen transparently to the
//! operator, as the operator only needs to return a BLOCKED result and call the callback using the InterruptState.
//! NO_INTERRUPTS: No blocking mode is specified, an error will be thrown when the operator blocks. Should only be used
//!                when manually calling operators of which is known they will never block.
//! TASK:          A weak pointer to a task is provided. On the callback, this task will be signalled. If the Task has
//!                been deleted, this callback becomes a NOP. This is the preferred way to await blocked pipelines.
//! BLOCKING:	   The caller has blocked awaiting some synchronization primitive to wait for the callback.
enum class InterruptMode : uint8_t { NO_INTERRUPTS, TASK, BLOCKING };

//! Synchronization primitive used to await a callback in InterruptMode::BLOCKING.
struct InterruptDoneSignalState {
	//! Called by the callback to signal the interrupt is over
	void Signal();
	//! Await the callback signalling the interrupt is over
	void Await();

protected:
	annotated_mutex lock;
	std::condition_variable cv;
	bool done = false;
};

//! State required to make the callback after some asynchronous operation within an operator source / sink.
class InterruptState {
public:
	//! Default interrupt state will be set to InterruptMode::NO_INTERRUPTS and throw an error on use of Callback()
	InterruptState();
	//! Register the task to be interrupted and set mode to InterruptMode::TASK, the preferred way to handle interrupts
	explicit InterruptState(weak_ptr<Task> task);
	//! Register signal state and set mode to InterruptMode::BLOCKING, used for code paths without Task.
	explicit InterruptState(weak_ptr<InterruptDoneSignalState> done_signal);

	//! Perform the callback to indicate the Interrupt is over
	DUCKDB_API void Callback() const;

	//! Whether Callback() can be invoked, i.e. a task or signal state was registered
	bool CanCallback() const {
		return mode != InterruptMode::NO_INTERRUPTS;
	}

protected:
	//! Current interrupt mode
	InterruptMode mode;
	//! Task ptr for InterruptMode::TASK
	weak_ptr<Task> current_task;
	//! Signal state for InterruptMode::BLOCKING
	weak_ptr<InterruptDoneSignalState> signal_state;
};

class StateWithBlockableTasks {
public:
	void PreventBlocking() DUCKDB_REQUIRES(lock) {
		can_block = false;
	}

	void ResetBlocking() DUCKDB_REQUIRES(lock) {
		can_block = true;
		blocked_tasks.clear();
	}

	//! Add a task to 'blocked_tasks' before returning SourceResultType::BLOCKED (must hold the lock)
	bool BlockTask(const InterruptState &interrupt_state) DUCKDB_REQUIRES(lock) {
		if (can_block) {
			blocked_tasks.push_back(interrupt_state);
			return true;
		}
		return false;
	}

	bool CanBlock() const DUCKDB_REQUIRES(lock) {
		return can_block;
	}

	//! Take the blocked tasks so their callbacks can run after releasing the lock
	vector<InterruptState> TakeBlockedTasks() DUCKDB_REQUIRES(lock) {
		vector<InterruptState> result;
		result.swap(blocked_tasks);
		return result;
	}

	//! Unblock all tasks (must hold the lock)
	bool UnblockTasks() DUCKDB_REQUIRES(lock) {
		if (blocked_tasks.empty()) {
			return false;
		}
		// take the tasks first, so that a callback that throws leaves none behind to be called a second time
		auto tasks = TakeBlockedTasks();
		for (auto &entry : tasks) {
			entry.Callback();
		}
		return true;
	}

	SinkResultType BlockSink(const InterruptState &interrupt_state) DUCKDB_REQUIRES(lock) {
		return BlockTask(interrupt_state) ? SinkResultType::BLOCKED : SinkResultType::FINISHED;
	}

	SourceResultType BlockSource(const InterruptState &interrupt_state) DUCKDB_REQUIRES(lock) {
		return BlockTask(interrupt_state) ? SourceResultType::BLOCKED : SourceResultType::FINISHED;
	}

public:
	//! Global lock
	mutable annotated_mutex lock;

private:
	//! Whether we can block tasks
	bool can_block DUCKDB_GUARDED_BY(lock) = true;
	//! Tasks that are currently blocked
	mutable vector<InterruptState> blocked_tasks DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
