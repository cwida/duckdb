//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/convert_to_string.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

struct ConvertToString {
	template <class SRC>
	static inline string Operation(SRC input) {
		throw InternalException("Unrecognized type for ConvertToString %s", GetTypeId<SRC>());
	}
};

template <>
DUCKDB_API string ConvertToString::Operation(bool input);
template <>
DUCKDB_API string ConvertToString::Operation(int8_t input);
template <>
DUCKDB_API string ConvertToString::Operation(int16_t input);
template <>
DUCKDB_API string ConvertToString::Operation(int32_t input);
template <>
DUCKDB_API string ConvertToString::Operation(int64_t input);
template <>
DUCKDB_API string ConvertToString::Operation(uint8_t input);
template <>
DUCKDB_API string ConvertToString::Operation(uint16_t input);
template <>
DUCKDB_API string ConvertToString::Operation(uint32_t input);
template <>
DUCKDB_API string ConvertToString::Operation(uint64_t input);
template <>
DUCKDB_API string ConvertToString::Operation(hugeint_t input);
template <>
DUCKDB_API string ConvertToString::Operation(uhugeint_t input);
template <>
DUCKDB_API string ConvertToString::Operation(float input);
template <>
DUCKDB_API string ConvertToString::Operation(double input);
template <>
DUCKDB_API string ConvertToString::Operation(interval_t input);
template <>
DUCKDB_API string ConvertToString::Operation(date_t input);
template <>
DUCKDB_API string ConvertToString::Operation(dtime_t input);
template <>
DUCKDB_API string ConvertToString::Operation(timestamp_t input);
template <>
DUCKDB_API string ConvertToString::Operation(timestamp_ns_t input);
template <>
DUCKDB_API string ConvertToString::Operation(timestamp_tz_t input);
template <>
DUCKDB_API string ConvertToString::Operation(timestamp_tz_ns_t input);
template <>
DUCKDB_API string ConvertToString::Operation(string_t input);
template <>
DUCKDB_API string ConvertToString::Operation(bignum_t input);

} // namespace duckdb
