//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/batch_memory_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

class BatchMemoryManager : public StateWithBlockableTasks {
public:
	BatchMemoryManager(ClientContext &context, idx_t initial_memory_request)
	    : context(context), unflushed_memory_usage(0), min_batch_index(0), available_memory(0),
	      can_increase_memory(true) {
		memory_state = TemporaryMemoryManager::Get(context.GetContext()).Register(context.GetContext());
		SetMemorySize(initial_memory_request);
	}

private:
	ClientContext &context;
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

public:
	void SetMemorySize(idx_t size) {
		// request at most 1/4th of all available memory
		idx_t total_max_memory = BufferManager::GetBufferManager(context).GetOperatorMemoryLimit();
		idx_t request_cap = total_max_memory / 4;

		size = MinValue<idx_t>(size, request_cap);
		if (size <= available_memory) {
			return;
		}

		memory_state->SetRemainingSizeAndUpdateReservation(context.GetContext(), size);
		auto next_reservation = memory_state->GetReservation();
		if (available_memory >= next_reservation) {
			// we tried to ask for more memory but were declined
			// stop asking for more memory
			can_increase_memory = false;
		}
		available_memory = next_reservation;
	}

	void IncreaseMemory() {
		if (!can_increase_memory) {
			return;
		}
		SetMemorySize(available_memory * 2);
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

	idx_t GetMinimumBatchIndex() const {
		return min_batch_index;
	}

	void FinalCheck() {
		if (unflushed_memory_usage != 0) {
			throw InternalException("Unflushed memory usage is not zero at finalize but %llu",
			                        unflushed_memory_usage.load());
		}
	}
};

} // namespace duckdb
