//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/batch_memory_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

class BatchMemoryManager : public StateWithBlockableTasks {
public:
	//! Registers with the temporary memory manager only when the memory is first requested, so a sink whose pipeline
	//! has not started does not hold a reservation
	BatchMemoryManager(ClientContext &context, idx_t initial_memory_request)
	    : context(context), initial_memory_request(initial_memory_request), unflushed_memory_usage(0),
	      min_batch_index(0), available_memory(0), can_increase_memory(true) {
	}

private:
	ClientContext &context;
	//! The minimum memory per thread, which is also the smallest first request
	idx_t initial_memory_request;
	//! Temporary memory state
	unique_ptr<TemporaryMemoryState> memory_state;
	//! Total memory usage of unflushed rows
	atomic<idx_t> unflushed_memory_usage;
	//! Minimum batch size
	atomic<idx_t> min_batch_index;
	//! The available memory for unflushed rows
	atomic<idx_t> available_memory;
	//! Whether or not we can request additional memory
	bool can_increase_memory;
	//! Guards the reservation. Only the temporary memory manager's lock is taken while holding it, so it can be
	//! acquired under the pipeline's sink lock and under the blocked-task lock without forming a cycle
	annotated_mutex reservation_lock;

private:
	void SetMemorySize(idx_t size) DUCKDB_REQUIRES(reservation_lock) {
		// request at most 1/4th of all available memory
		idx_t total_max_memory = BufferManager::GetBufferManager(context).GetOperatorMemoryLimit();
		idx_t request_cap = total_max_memory / 4;

		size = MinValue<idx_t>(size, request_cap);
		if (!memory_state) {
			memory_state = TemporaryMemoryManager::Get(context).Register(context);
			size = MaxValue<idx_t>(size, MinValue<idx_t>(initial_memory_request, request_cap));
		}
		if (size <= available_memory) {
			return;
		}

		memory_state->SetRemainingSizeAndUpdateReservation(context, size);
		auto next_reservation = memory_state->GetReservation();
		if (available_memory >= next_reservation) {
			// we tried to ask for more memory but were declined
			// stop asking for more memory
			can_increase_memory = false;
		}
		available_memory = next_reservation;
	}

	void IncreaseMemory() DUCKDB_REQUIRES(reservation_lock) {
		if (!can_increase_memory) {
			return;
		}
		SetMemorySize(MaxValue<idx_t>(available_memory * 2, initial_memory_request));
	}

public:
	//! Requests memory for the given number of threads and returns how many threads that memory supports
	idx_t MaxThreads(idx_t source_max_threads) {
		const annotated_lock_guard<annotated_mutex> guard(reservation_lock);
		// try to request the initial memory per thread
		SetMemorySize(source_max_threads * initial_memory_request);
		// cap the concurrent threads working on this task based on the amount of available memory
		return MinValue<idx_t>(source_max_threads, available_memory / initial_memory_request + 1);
	}

	bool OutOfMemory(idx_t batch_index) {
#ifdef DUCKDB_ALTERNATIVE_VERIFY
		// alternative verify - always report that we are out of memory to test this code path
		return true;
#else
		if (unflushed_memory_usage >= available_memory) {
			const annotated_lock_guard<annotated_mutex> guard(lock);
			if (batch_index > min_batch_index) {
				// exceeded available memory and we are not the minimum batch index- try to increase it
				const annotated_lock_guard<annotated_mutex> reservation_guard(reservation_lock);
				IncreaseMemory();
				if (unflushed_memory_usage >= available_memory) {
					// STILL out of memory
					return true;
				}
			}
		}
		return false;
#endif
	}

	void UpdateMinBatchIndex(idx_t current_min_batch_index) {
		if (min_batch_index >= current_min_batch_index) {
			return;
		}
		const annotated_lock_guard<annotated_mutex> guard(lock);
		auto new_batch_index = MaxValue<idx_t>(min_batch_index, current_min_batch_index);
		if (new_batch_index != min_batch_index) {
			// new batch index! unblock all tasks
			min_batch_index = new_batch_index;
			UnblockTasks();
		}
	}

	idx_t AvailableMemory() const {
		return available_memory;
	}

	idx_t GetUnflushedMemory() {
		return unflushed_memory_usage;
	}

	void IncreaseUnflushedMemory(idx_t memory_increase) {
		unflushed_memory_usage += memory_increase;
	}

	void ReduceUnflushedMemory(idx_t memory_reduction) {
		if (unflushed_memory_usage < memory_reduction) {
			throw InternalException("Reducing unflushed memory usage below zero!?");
		} else {
			unflushed_memory_usage -= memory_reduction;
		}
	}

	bool IsMinimumBatchIndex(idx_t batch_index) {
		return batch_index <= min_batch_index;
	}

	void FinalCheck() {
		if (unflushed_memory_usage != 0) {
			throw InternalException("Unflushed memory usage is not zero at finalize but %llu",
			                        unflushed_memory_usage.load());
		}
	}

	//! Gives the reservation back once everything is flushed. Optional: the destructor gives it back at the end of
	//! the query, but releasing early lets later sinks in the same query, e.g. EXPORT DATABASE, use the memory
	void Release() {
		const annotated_lock_guard<annotated_mutex> guard(reservation_lock);
		memory_state.reset();
		available_memory = 0;
		can_increase_memory = false;
	}
};

} // namespace duckdb
