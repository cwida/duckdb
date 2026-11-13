//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/scan_vector_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class ScanVectorType {
	SCAN_ENTIRE_VECTOR,
	SCAN_FLAT_VECTOR,
	//! Select into a flat vector while preserving the original row positions
	SCAN_FLAT_VECTOR_PRESERVE_POSITIONS
};

} // namespace duckdb
