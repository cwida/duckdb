//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/string_cast.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

struct date_t;

//! StringCast
class StringHeap;

struct StringCast {
	template <class SRC>
	static inline string_t Operation(SRC input, StringHeap &heap) {
		throw NotImplementedException("Unimplemented type for string cast!");
	}
};

template <>
DUCKDB_API duckdb::string_t StringCast::Operation(bool input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(int8_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(int16_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(int32_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(int64_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(uint8_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(uint16_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(uint32_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(uint64_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(hugeint_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(uhugeint_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(float input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(double input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(interval_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(duckdb::string_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(date_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(dtime_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(dtime_ns_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(timestamp_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(timestamp_ns_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(timestamp_ms_t input, StringHeap &heap);
template <>
DUCKDB_API duckdb::string_t StringCast::Operation(timestamp_sec_t input, StringHeap &heap);

template <>
duckdb::string_t StringCast::Operation(date_t input, StringHeap &heap);
template <>
duckdb::string_t StringCast::Operation(dtime_tz_t input, StringHeap &heap);
template <>
duckdb::string_t StringCast::Operation(timestamp_tz_t input, StringHeap &heap);
template <>
duckdb::string_t StringCast::Operation(timestamp_tz_ns_t input, StringHeap &heap);

} // namespace duckdb
