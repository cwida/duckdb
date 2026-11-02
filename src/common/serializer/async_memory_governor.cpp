#include "duckdb/common/serializer/async_memory_governor.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

ManagedAsyncMemoryGovernor::ManagedAsyncMemoryGovernor(ClientContext &client_context_p)
    : client_context(client_context_p) {
	auto &scheduler = TaskScheduler::GetScheduler(client_context);
	auto regular_threads = MaxValue<idx_t>(NumericCast<idx_t>(scheduler.NumberOfThreads()), 1);
	auto async_threads = NumericCast<idx_t>(scheduler.NumberOfAsyncThreads());
	auto max_memory = BufferManager::GetBufferManager(client_context).GetMaxMemory();
	max_pending_bytes = MinValue(ManagedAsyncMemoryConfig::MAX_PENDING_BYTES_PER_THREAD * regular_threads,
	                             max_memory / ManagedAsyncMemoryConfig::MAX_PENDING_MEMORY_LIMIT_DIVISOR);
	min_pending_bytes =
	    MinValue(max_pending_bytes, MinValue(ManagedAsyncMemoryConfig::MIN_PENDING_BYTES_PER_THREAD * regular_threads,
	                                         max_memory / ManagedAsyncMemoryConfig::MIN_PENDING_MEMORY_LIMIT_DIVISOR));
	// A reservation is only useful when drain tasks run asynchronously; synchronous draining bounds itself.
	if (async_threads > 0 && max_pending_bytes > 0) {
		memory_state = TemporaryMemoryManager::Get(client_context).Register(client_context);
		memory_state->SetMinimumReservation(min_pending_bytes);
		memory_state->SetZero();
	}
}

ManagedAsyncMemoryGovernor::~ManagedAsyncMemoryGovernor() = default;

bool ManagedAsyncMemoryGovernor::IsActive() const {
	return memory_state != nullptr;
}

void ManagedAsyncMemoryGovernor::UpdateReservation(idx_t current_pending_bytes) {
	if (!memory_state || current_pending_bytes == 0) {
		return;
	}

	auto current_reservation = memory_state->GetReservation();
	while (current_pending_bytes > MinValue(current_reservation, max_pending_bytes)) {
		idx_t next_request;
		if (memory_request_bytes > current_reservation) {
			// TMM did not fully grant the previous request. Keep retrying it on later growth checks.
			next_request = memory_request_bytes;
		} else if (memory_request_bytes == 0) {
			// Grow coarsely and only release on Release().
			// Repeatedly shrinking here would touch shared TMM state on the registration hot path.
			next_request = min_pending_bytes;
		} else if (memory_request_bytes >= max_pending_bytes) {
			return;
		} else if (memory_request_bytes > max_pending_bytes / 2) {
			next_request = max_pending_bytes;
		} else {
			next_request = memory_request_bytes * 2;
		}
		next_request = MinValue(MaxValue(next_request, min_pending_bytes), max_pending_bytes);
		if (next_request <= memory_request_bytes) {
			return;
		}

		auto previous_reservation = current_reservation;
		memory_state->SetRemainingSizeAndUpdateReservation(client_context, next_request);
		memory_request_bytes = next_request;
		current_reservation = memory_state->GetReservation();
		if (current_reservation <= previous_reservation) {
			return;
		}
		if (current_reservation < next_request) {
			return;
		}
	}
}

idx_t ManagedAsyncMemoryGovernor::BackpressureBudget() const {
	if (!memory_state) {
		return NumericLimits<idx_t>::Maximum();
	}
	auto reservation = MinValue(memory_state->GetReservation(), max_pending_bytes);
	// If TMM only grants a tiny reservation, do not retain an async backlog. This makes low-memory execution
	// behave close to synchronous draining, but automatically allows overlap again if the reservation grows later.
	if (reservation < ManagedAsyncMemoryConfig::MIN_RESERVATION_FOR_BACKLOG) {
		return 0;
	}
	return reservation;
}

void ManagedAsyncMemoryGovernor::Release() {
	if (!memory_state || memory_request_bytes == 0) {
		return;
	}
	memory_state->SetZero();
	memory_request_bytes = 0;
}

} // namespace duckdb
