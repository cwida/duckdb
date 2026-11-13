//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi/cast/generic_cast.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/date.hpp"

#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/capi/cast/utils.hpp"
#include "duckdb/main/capi/cast/from_decimal.hpp"

namespace duckdb {

template <class RESULT_TYPE, class OP = duckdb::TryCast>
RESULT_TYPE GetInternalCValue(duckdb_result *result, idx_t col, idx_t row) {
	if (!CanFetchValue(result, col, row)) {
		return FetchDefaultValue::Operation<RESULT_TYPE>();
	}
	switch (result->deprecated_columns[col].deprecated_type) {
	case DUCKDB_TYPE_BOOLEAN:
		return TryCastCInternal<bool, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_TINYINT:
		return TryCastCInternal<int8_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_SMALLINT:
		return TryCastCInternal<int16_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_INTEGER:
		return TryCastCInternal<int32_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_BIGINT:
		return TryCastCInternal<int64_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_UTINYINT:
		return TryCastCInternal<uint8_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_USMALLINT:
		return TryCastCInternal<uint16_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_UINTEGER:
		return TryCastCInternal<uint32_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_UBIGINT:
		return TryCastCInternal<uint64_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_FLOAT:
		return TryCastCInternal<float, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_DOUBLE:
		return TryCastCInternal<double, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_DATE:
		return TryCastCInternal<date_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_TIME:
		return TryCastCInternal<dtime_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_TIME_NS:
		return TryCastCInternal<dtime_ns_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_TIMESTAMP:
		return TryCastCInternal<timestamp_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_HUGEINT:
		return TryCastCInternal<hugeint_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_UHUGEINT:
		return TryCastCInternal<uhugeint_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_DECIMAL:
		return TryCastDecimalCInternal<RESULT_TYPE>(result, col, row);
	case DUCKDB_TYPE_INTERVAL:
		return TryCastCInternal<interval_t, RESULT_TYPE, OP>(result, col, row);
	case DUCKDB_TYPE_VARCHAR:
		return TryCastCInternal<char *, RESULT_TYPE, FromCStringCastWrapper<OP>>(result, col, row);
	case DUCKDB_TYPE_NUMERIC:
		// materialized as its decimal text
		return TryCastCInternal<char *, RESULT_TYPE, FromCStringCastWrapper<OP>>(result, col, row);
	case DUCKDB_TYPE_BLOB:
		return TryCastCInternal<duckdb_blob, RESULT_TYPE, FromCBlobCastWrapper>(result, col, row);
	default: { // LCOV_EXCL_START
		// Invalid type for C to C++ conversion. Internally, we set the null mask to NULL.
		// This is a deprecated code path. Use the Vector Interface for nested and complex types.
		return FetchDefaultValue::Operation<RESULT_TYPE>();
	} // LCOV_EXCL_STOP
	}
}

} // namespace duckdb
