//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/buffered_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/common/enums/query_result_state.hpp"
#include "duckdb/common/enums/result_lifetime.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/thread_annotation.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_unit.hpp"

namespace duckdb {

class QueryResult;
class ClientContextLock;

//! A blocked sink. Holds the InterruptState and owns the finished unit it could not append.
struct BlockedSink {
public:
	InterruptState state;
	unique_ptr<ResultUnit> pending_unit;

public:
	//! The bytes the parked unit counts against the cap, zero once the unit has been deposited
	idx_t PendingBytes() const {
		return pending_unit ? pending_unit->byte_size : 0;
	}
};

class BufferedData {
protected:
	enum class Type { SIMPLE, BATCHED };

public:
	//! Runs the format's InitGlobal on the constructing thread, before any worker sees it. A null format means chunks
	BufferedData(Type type, ClientContext &context, ResultLifetime lifetime, ResultFormatContext format_context,
	             shared_ptr<ResultFormat> format);
	virtual ~BufferedData();

public:
	//! The retention this buffer serves. UNDECIDED until the consumer's first call
	ResultLifetime Lifetime() const {
		return lifetime;
	}
	//! The first decision stands
	ResultLifetime Decide(ResultLifetime decision);
	//! Fixed at construction; safe to read without synchronization
	ResultFormat &Format() const {
		D_ASSERT(format);
		return *format;
	}
	//! Fixed at construction; safe to read without synchronization
	ResultFormatGlobalState &FormatState() const {
		D_ASSERT(format_state);
		return *format_state;
	}
	const shared_ptr<ResultFormat> &SharedFormat() const {
		return format;
	}
	const shared_ptr<ResultFormatGlobalState> &SharedFormatState() const {
		return format_state;
	}
	//! Fixed at construction; safe to read without synchronization
	const ResultFormatContext &FormatContext() const {
		return format_context;
	}
	//! Choose draining, as every fetch-shaped call does. Throws when the result is being materialized
	void DecideDraining();
	//! Park a producer until the retention is decided. False when it already is
	bool ParkUndecided(const InterruptState &blocked_sink);
	//! Whether the engine waits on the consumer: a producer is parked for the retention decision, or for
	//! space that only a pop frees
	bool WaitsOnConsumer();
	//! Blocking call that executes tasks on the calling thread until a unit is buffered or execution reaches a
	//! terminal state.
	QueryResultState ReplenishBuffer(ClientContextLock &context_lock, QueryResult &result);
	//! Lend the calling thread to production: settle draining, wake parked producers and run at most one
	//! task slice. Reports whether a unit is poppable. Never waits
	QueryResultState Participate(ClientContextLock &context_lock, QueryResult &result);
	//! Reports whether a unit is poppable, else where execution stands. Runs no task, never waits
	QueryResultState Poll(ClientContextLock &context_lock, QueryResult &result);
	virtual unique_ptr<ResultUnit> Scan() = 0;
	virtual void UnblockSinks() = 0;
	shared_ptr<ClientContext> GetContext() {
		return context.lock();
	}
	//! The highest number of bytes the buffer ever queued. This is what the cap governs
	virtual idx_t PeakBufferedBytes() = 0;
	//! Queued bytes plus the units parked producers hold, as system.peak_streaming_buffer_size reports
	virtual idx_t PeakStreamingBytes() = 0;
	//! Whether a producer is parked for space.
	virtual bool HasBlockedSink() = 0;
	//! Whether a unit is ready for the consumer to pop.
	virtual bool HasObservableUnit() = 0;
	//! Debug assert that no blocked sinks exist.
	virtual void AssertNoBlockedSinks() = 0;
	//! An owned, exactly-sized copy: the producer reuses its output chunk, and the copy's GetDataSize is what the
	//! cap counts
	static unique_ptr<DataChunk> CopyForBuffering(DataChunk &chunk);
	bool Closed() const {
		if (context.expired()) {
			return true;
		}
		auto c = context.lock();
		return c == nullptr;
	}
	void Close() {
		context.reset();
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		if (TARGET::TYPE != type) {
			InternalException::Throw("Failed to cast buffered data to type - buffered data type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (TARGET::TYPE != type) {
			InternalException::Throw("Failed to cast buffered data to type - buffered data type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	//! Record on the result that it is no longer the connection's active query, and report it as an
	//! error state
	static QueryResultState Cancelled(QueryResult &result);
	//! Whether the blocking replenish can stop: a unit is poppable, and the buffer will
	//! not accept more input right now
	virtual bool ReplenishSatisfied() = 0;
	//! Pops below this mark restart blocked sinks. The floor keeps it reachable for tiny buffers
	static idx_t LowWaterMark(idx_t capacity);

protected:
	Type type;
	//! This is weak to avoid a cyclical reference
	weak_ptr<ClientContext> context;
	//! The maximum amount of memory we should keep buffered
	const idx_t total_buffer_size;
	//! Protect against populate/fetch race condition
	annotated_mutex glock;
	//! Read unlocked on the producers' fast path; written once, under glock, by Decide
	atomic<ResultLifetime> lifetime;
	//! Producers parked with their first chunk unconsumed, until the retention is decided
	vector<InterruptState> undecided_sinks DUCKDB_GUARDED_BY(glock);
	const ResultFormatContext format_context;
	//! Set at construction and never changed afterwards, so workers read these without synchronization
	shared_ptr<ResultFormat> format;
	shared_ptr<ResultFormatGlobalState> format_state;
};

} // namespace duckdb
