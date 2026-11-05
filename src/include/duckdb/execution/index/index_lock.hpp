//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/index_lock.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/index/bound_index.hpp"

namespace duckdb {

struct DUCKDB_CAPABILITY("mutex") DUCKDB_SCOPED_CAPABILITY IndexLock {
public:
	explicit IndexLock(const BoundIndex &index) DUCKDB_ACQUIRE(index.lock)
	    : index_guard(index.lock), locked_index(index) {
	}
	~IndexLock() DUCKDB_RELEASE() = default;

	IndexLock(const IndexLock &) = delete;
	IndexLock &operator=(const IndexLock &) = delete;

	//! Verify that a borrowed guard holds this index's mutex.
	void AssertHeld(const BoundIndex &index) const DUCKDB_ASSERT_CAPABILITY(this) DUCKDB_ASSERT_CAPABILITY(index.lock) {
		D_ASSERT(&locked_index == &index);
	}

private:
	lock_guard<mutex> index_guard;
	const BoundIndex &locked_index;
};

} // namespace duckdb
