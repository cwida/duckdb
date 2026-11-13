//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate/numeric_aggregates.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

//! Aggregates over NUMERIC that work on the run-time representation of their input instead of the canonical blobs
struct NumericAggregates {
	//! min/max: the state holds the extreme as an exact unscaled value, compared natively with the typed input
	DUCKDB_API static AggregateFunction GetMinMax(bool is_min);
	//! sum: exact in a 128-bit accumulator at the scale the values need; runs are summed in the input representation
	DUCKDB_API static AggregateFunction GetSum();
	//! The average as a DOUBLE, like the average of a DECIMAL
	DUCKDB_API static AggregateFunction GetAvg();
	//! first/last/any_value: only the recorded rows are converted
	DUCKDB_API static AggregateFunction GetFirst(bool last, bool skip_nulls);
};

} // namespace duckdb
