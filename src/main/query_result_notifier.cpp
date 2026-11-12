#include "duckdb/main/query_result_notifier.hpp"

namespace duckdb {

QueryResultNotifier::QueryResultNotifier(std::function<void()> callback_p) : callback(std::move(callback_p)) {
}

void QueryResultNotifier::Notify() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (!callback) {
		return;
	}
	// The callback is a wake-up, not part of the query: its failure must not change the query's outcome
	try {
		callback();
	} catch (...) { // NOLINT
	}
}

void QueryResultNotifier::Clear() {
	// Destroyed once the lock is released: what the callback captured is user state
	std::function<void()> cleared;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		cleared = std::move(callback);
		callback = nullptr;
	}
}

} // namespace duckdb
