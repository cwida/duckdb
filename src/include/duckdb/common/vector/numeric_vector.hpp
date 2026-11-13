//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/numeric_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"

namespace duckdb {

//! Helpers for NUMERIC vectors, whose values are either typed (a shredded vector holding one exact representation) or
//! canonical blobs
struct NumericVector {
	//! The typed representation of a NUMERIC vector: the shredded child, or a decoded constant; false when the vector
	//! holds canonical blobs that are not a constant
	DUCKDB_API static bool TryGetTyped(const Vector &vector, unique_ptr<Vector> &typed);
	//! Both operands of a comparison in one exact representation that holds the values of both; false when either side
	//! has no typed representation
	DUCKDB_API static bool TryNativePair(const Vector &left, const Vector &right, unique_ptr<Vector> &left_typed,
	                                     unique_ptr<Vector> &right_typed);
	//! The exact range of the values of a vector of an exact representation; false for representations beyond 128 bits
	DUCKDB_API static bool TryComputeRange(const Vector &typed, idx_t count, ShreddedNumericRange &range);
	//! The exact range of a NUMERIC vector with a typed representation: recorded on the vector, or computed from the
	//! values and recorded; false for canonical vectors and representations beyond 128 bits
	DUCKDB_API static bool TryGetRange(const Vector &vector, idx_t count, ShreddedNumericRange &range);
	//! Records the exact range of a shredded NUMERIC vector
	DUCKDB_API static void SetRange(Vector &vector, const ShreddedNumericRange &range);
};

} // namespace duckdb
