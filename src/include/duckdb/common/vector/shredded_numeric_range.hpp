//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/shredded_numeric_range.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/hugeint.hpp"

namespace duckdb {

//! The exact range of the values of a NUMERIC vector, as unscaled values at the scale of its representation
struct ShreddedNumericRange {
	bool valid = false;
	uint32_t scale = 0;
	hugeint_t min;
	hugeint_t max;
};

} // namespace duckdb
