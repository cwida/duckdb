//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_result_notifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/thread_annotation.hpp"

#include <functional>

namespace duckdb {

//! Runs the consumer's notify callback (QueryParameters::notify_callback) for one query, from its beginning until it
//! ends
class QueryResultNotifier {
public:
	explicit QueryResultNotifier(std::function<void()> callback_p);

public:
	//! Runs the callback unless the query has ended. An exception it throws is swallowed
	void Notify() DUCKDB_EXCLUDES(lock);
	//! Once Clear returns, the callback is not running and never runs again
	void Clear() DUCKDB_EXCLUDES(lock);

private:
	//! Held while the callback runs, which makes Clear a barrier
	annotated_mutex lock;
	std::function<void()> callback DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
