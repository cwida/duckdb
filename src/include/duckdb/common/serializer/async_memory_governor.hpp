//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/async_memory_governor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class TemporaryMemoryState;

//! Memory policy shared by the managed async queues.
struct ManagedAsyncMemoryConfig {
	//! Maximum queued async bytes retained per regular execution thread.
	static constexpr idx_t MAX_PENDING_BYTES_PER_THREAD = 64ULL * 1024ULL * 1024ULL;
	//! Minimum async reservation requested per regular execution thread.
	static constexpr idx_t MIN_PENDING_BYTES_PER_THREAD = 8ULL * 1024ULL * 1024ULL;
	//! Maximum fraction of the memory limit the async backlog may claim.
	static constexpr idx_t MAX_PENDING_MEMORY_LIMIT_DIVISOR = 8ULL;
	//! Maximum fraction of the memory limit the minimum async reservation may claim.
	static constexpr idx_t MIN_PENDING_MEMORY_LIMIT_DIVISOR = 16ULL;
	//! Below this reservation, do not retain an async backlog (behave close to synchronous draining).
	static constexpr idx_t MIN_RESERVATION_FOR_BACKLOG = 8ULL * 1024ULL * 1024ULL;
};

//! Shared TemporaryMemoryManager reservation governor for the managed async queues.
//! Encapsulates the coarse-growth reservation heuristic and backpressure budget so each queue bounds its
//! queued and in-flight backlog through the same memory logic.
class ManagedAsyncMemoryGovernor {
public:
	explicit ManagedAsyncMemoryGovernor(ClientContext &client_context);
	~ManagedAsyncMemoryGovernor();

	ManagedAsyncMemoryGovernor(const ManagedAsyncMemoryGovernor &) = delete;
	ManagedAsyncMemoryGovernor &operator=(const ManagedAsyncMemoryGovernor &) = delete;

	//! Whether a TemporaryMemoryState reservation is tracking this queue's backlog.
	bool IsActive() const;
	//! Grow the reservation coarsely until it covers current_pending_bytes; released only on Release().
	void UpdateReservation(idx_t current_pending_bytes);
	//! Current async backlog budget after applying the fixed cap, or 0 when memory is too tight to retain a backlog.
	idx_t BackpressureBudget() const;
	//! Release the reservation; further UpdateReservation calls may grow it again.
	void Release();

private:
	ClientContext &client_context;
	//! Temporary memory reservation state used to limit queued async data. Absent when draining synchronously.
	unique_ptr<TemporaryMemoryState> memory_state;
	//! Last remaining-size request sent to TemporaryMemoryManager. Grows monotonically until Release().
	idx_t memory_request_bytes = 0;
	//! Minimum TemporaryMemoryManager reservation while work is outstanding.
	idx_t min_pending_bytes = 0;
	//! Hard cap over the TemporaryMemoryState reservation.
	idx_t max_pending_bytes = 0;
};

} // namespace duckdb
