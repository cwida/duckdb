#include "duckdb/common/thread.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/original/std/sstream.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/time_point.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

#ifndef DUCKDB_NO_THREADS
void ThreadUtil::SleepMs(idx_t sleep_ms, optional_ptr<ClientContext> context) {
	auto start_time = TimePoint::Tick();
	static constexpr idx_t DEFAULT_SLEEP_INTERVAL_MS = 100;

	while (true) {
		if (context) {
			context->BlockingInterruptCheck();
		}
		auto elapsed_ms = static_cast<idx_t>(start_time.ElapsedMillis());
		if (elapsed_ms >= sleep_ms) {
			break;
		}
		auto remaining_ms = sleep_ms - elapsed_ms;
		std::this_thread::sleep_for(milliseconds(MinValue(remaining_ms, DEFAULT_SLEEP_INTERVAL_MS)));
	}
}

void ThreadUtil::SleepMicroSeconds(idx_t micros) {
	std::this_thread::sleep_for(microseconds(micros));
}

thread_id ThreadUtil::GetThreadId() {
	return std::this_thread::get_id();
}

string ThreadUtil::GetThreadIdString() {
	std::ostringstream ss;
	ss << std::this_thread::get_id();
	return ss.str();
}

#else

void ThreadUtil::SleepMs(idx_t sleep_ms, optional_ptr<ClientContext>) {
	throw InvalidInputException("ThreadUtil::SleepMs requires DuckDB to be compiled with thread support");
}

void ThreadUtil::SleepMicroSeconds(idx_t micros) {
	throw InvalidInputException("ThreadUtil::SleepMicroSeconds requires DuckDB to be compiled with thread support");
}

thread_id ThreadUtil::GetThreadId() {
	return 0;
}

string ThreadUtil::GetThreadIdString() {
	return "0";
}

#endif
} // namespace duckdb
