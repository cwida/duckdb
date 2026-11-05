//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/radix_ht_adaptivity.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class RadixHTGlobalSinkState;
class RadixHTLocalSinkState;

//! Policy for the local sink HT. Called at fill-cycle boundaries without memory pressure.
class RadixHTAdaptivity {
public:
	//! Grow a profitable table within the memory budget, resuming lookups on success.
	static bool TryGrow(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate);
	//! Consider skipping lookups using the completed cycle, before abandonment.
	static void MaybeSkipLookups(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate);
	//! Periodically retry lookups at the existing capacity, after abandonment.
	static void MaybeResumeLookups(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate);

	//! Minimum input before skipping lookups and between subsequent retries
	static constexpr idx_t LOOKUP_SAMPLE_SIZE = 1048576;

private:
	static constexpr double GROWTH_DUPLICATE_BYTES_FACTOR = 4.0;
	static constexpr double MINIMUM_SMALL_TABLE_REDUCTION = 0.05;
};

} // namespace duckdb
