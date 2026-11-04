//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/segment_base.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

template <class T>
class SegmentBase {
private:
	friend T;

	explicit SegmentBase(idx_t count) : count(count) {
	}

public:
	//! The amount of entries in this storage chunk
	atomic<idx_t> count;
};

} // namespace duckdb
