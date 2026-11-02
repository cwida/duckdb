//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/interpolate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

//	Linear interpolation between two values
struct InterpolateOperator {
	template <typename TARGET_TYPE>
	static inline TARGET_TYPE Operation(const TARGET_TYPE &lo, const double d, const TARGET_TYPE &hi) {
		const auto delta = static_cast<double>(hi) - static_cast<double>(lo);
		return LossyNumericCast<TARGET_TYPE>(static_cast<double>(lo) + delta * d);
	}
};

template <>
double InterpolateOperator::Operation(const double &lo, const double d, const double &hi);
template <>
dtime_t InterpolateOperator::Operation(const dtime_t &lo, const double d, const dtime_t &hi);
template <>
dtime_tz_t InterpolateOperator::Operation(const dtime_tz_t &lo, const double d, const dtime_tz_t &hi);
template <>
timestamp_t InterpolateOperator::Operation(const timestamp_t &lo, const double d, const timestamp_t &hi);
template <>
hugeint_t InterpolateOperator::Operation(const hugeint_t &lo, const double d, const hugeint_t &hi);
template <>
uhugeint_t InterpolateOperator::Operation(const uhugeint_t &lo, const double d, const uhugeint_t &hi);
template <>
interval_t InterpolateOperator::Operation(const interval_t &lo, const double d, const interval_t &hi);

} // namespace duckdb
