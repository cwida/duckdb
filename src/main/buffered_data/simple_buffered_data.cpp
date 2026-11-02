#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

SimpleBufferedData::SimpleBufferedData(ClientContext &context, ResultLifetime lifetime,
                                       ResultFormatContext format_context, shared_ptr<ResultFormat> format)
    : BufferedData(BufferedData::Type::SIMPLE, context, lifetime, std::move(format_context), std::move(format)),
      buffered_count(0), buffer_size(total_buffer_size) {
}

SimpleBufferedData::~SimpleBufferedData() {
}

bool SimpleBufferedData::BufferSaturated() {
	annotated_lock_guard<annotated_mutex> lock(glock);
	return buffered_count >= BufferSize() || !blocked_sinks.empty();
}

idx_t SimpleBufferedData::PeakBufferedBytes() {
	annotated_lock_guard<annotated_mutex> lock(glock);
	return peak_buffered_bytes;
}

idx_t SimpleBufferedData::PeakStreamingBytes() {
	annotated_lock_guard<annotated_mutex> lock(glock);
	return peak_streaming_bytes;
}

bool SimpleBufferedData::HasBlockedSink() {
	annotated_lock_guard<annotated_mutex> lock(glock);
	return !blocked_sinks.empty();
}

bool SimpleBufferedData::HasObservableUnit() {
	annotated_lock_guard<annotated_mutex> lock(glock);
	// Readiness is the unit queue, never the byte count: units with rows but zero data bytes exist
	return !unread_units.empty();
}

void SimpleBufferedData::CollectRestartableSinks(annotated_lock_guard<annotated_mutex> &lock,
                                                 vector<BlockedSink> &to_unblock) {
	D_ASSERT(to_unblock.empty());
	// Reserve first so a failed allocation loses no blocked sink
	to_unblock.reserve(blocked_sinks.size());
	while (!blocked_sinks.empty()) {
		auto &front = blocked_sinks.front();
		// Sinks restart in FIFO order. Stop at the first unit that does not fit yet
		if (buffered_count > 0 && buffered_count + front.PendingBytes() > BufferSize()) {
			break;
		}
		// Deposit the parked unit, so it is visible before the producer wakes.
		// Parks always carry their unit; the guard keeps a null from corrupting the queue
		if (front.pending_unit) {
			const idx_t pending_bytes = front.pending_unit->byte_size;
			parked_bytes -= pending_bytes;
			buffered_count += pending_bytes;
			unread_units.push(std::move(front.pending_unit));
			peak_buffered_bytes = MaxValue<idx_t>(peak_buffered_bytes, buffered_count);
			peak_streaming_bytes = MaxValue<idx_t>(peak_streaming_bytes, buffered_count + parked_bytes);
		}
		to_unblock.push_back(std::move(front));
		blocked_sinks.pop();
	}
}

void SimpleBufferedData::InvokeUnblocks(const vector<BlockedSink> &to_unblock) {
	// Invoked outside glock. Callback() takes the executor lock. A throw here
	// terminates the query, and teardown reclaims the parked tasks
	for (auto &blocked : to_unblock) {
		blocked.state.Callback();
	}
}

void SimpleBufferedData::UnblockSinks() {
	auto cc = context.lock();
	if (!cc) {
		return;
	}
	(void)cc;

	if (buffered_count >= BufferSize()) {
		return;
	}
	// Reschedule enough blocked sinks to populate the buffer
	vector<BlockedSink> to_unblock;
	{
		annotated_lock_guard<annotated_mutex> lock(glock);
		CollectRestartableSinks(lock, to_unblock);
	}
	InvokeUnblocks(to_unblock);
}

void SimpleBufferedData::AssertNoBlockedSinks() {
#ifdef D_ASSERT_IS_ENABLED
	annotated_lock_guard<annotated_mutex> lock(glock);
	D_ASSERT(blocked_sinks.empty());
#endif
}

unique_ptr<ResultUnit> SimpleBufferedData::Scan() {
	if (Closed()) {
		return nullptr;
	}

	unique_ptr<ResultUnit> unit;
	vector<BlockedSink> to_unblock;
	{
		annotated_lock_guard<annotated_mutex> lock(glock);
		if (unread_units.empty()) {
			Close();
			return nullptr;
		}
		unit = std::move(unread_units.front());
		unread_units.pop();
		buffered_count -= unit->byte_size;
		// The pop restarts blocked producers below the low-water mark
		if (buffered_count < LowWaterMark(BufferSize())) {
			CollectRestartableSinks(lock, to_unblock);
		}
	}
	InvokeUnblocks(to_unblock);
	return unit;
}

bool SimpleBufferedData::AppendOrBlock(unique_ptr<ResultUnit> unit, const InterruptState &blocked_sink) {
	const idx_t unit_data_size = unit->byte_size;
	annotated_lock_guard<annotated_mutex> lock(glock);
	// The buffer admits a unit that fits, and always one unit when empty
	if (buffered_count > 0 && buffered_count + unit_data_size > BufferSize()) {
		// Park holding the finished unit. Restart selection deposits it at wake time
		parked_bytes += unit_data_size;
		peak_streaming_bytes = MaxValue<idx_t>(peak_streaming_bytes, buffered_count + parked_bytes);
		blocked_sinks.push(BlockedSink {blocked_sink, std::move(unit)});
		return true;
	}
	unread_units.push(std::move(unit));
	buffered_count += unit_data_size;
	peak_buffered_bytes = MaxValue<idx_t>(peak_buffered_bytes, buffered_count);
	peak_streaming_bytes = MaxValue<idx_t>(peak_streaming_bytes, buffered_count + parked_bytes);
	return false;
}

} // namespace duckdb
