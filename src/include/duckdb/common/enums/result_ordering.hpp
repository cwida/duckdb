//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/result_ordering.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! How the consumer-visible chunk order is established
enum class ResultOrdering : uint8_t {
	//! No order guarantee: a parallel sink stores chunks as they arrive
	UNORDERED,
	//! Source order, preserved by sinking through a single thread
	SOURCE_ORDERED,
	//! Source order, restored from batch indexes under a parallel sink
	BATCH_INDEX_ORDERED
};

} // namespace duckdb
