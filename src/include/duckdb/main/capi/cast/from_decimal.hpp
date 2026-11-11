//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi/capi_cast_from_decimal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/capi/cast/utils.hpp"

namespace duckdb {

//! DECIMAL -> ?
template <class RESULT_TYPE>
bool CastDecimalCInternal(duckdb_result *source, RESULT_TYPE &result, idx_t col, idx_t row) {
	auto result_data = (duckdb::DuckDBResultData *)source->internal_data;
	auto &query_result = result_data->result;
	auto &source_type = query_result->GetTypes()[col];
	auto width = duckdb::DecimalType::GetWidth(source_type);
	auto scale = duckdb::DecimalType::GetScale(source_type);
	auto source_value = UnsafeFetch<hugeint_t>(source, col, row);
	CastParameters parameters;
	switch (source_type.InternalType()) {
	case duckdb::PhysicalType::INT16:
		return duckdb::TryCastFromDecimal::Operation<int16_t, RESULT_TYPE>(static_cast<int16_t>(source_value), result,
		                                                                   parameters, width, scale);
	case duckdb::PhysicalType::INT32:
		return duckdb::TryCastFromDecimal::Operation<int32_t, RESULT_TYPE>(static_cast<int32_t>(source_value), result,
		                                                                   parameters, width, scale);
	case duckdb::PhysicalType::INT64:
		return duckdb::TryCastFromDecimal::Operation<int64_t, RESULT_TYPE>(static_cast<int64_t>(source_value), result,
		                                                                   parameters, width, scale);
	case duckdb::PhysicalType::INT128:
		return duckdb::TryCastFromDecimal::Operation<hugeint_t, RESULT_TYPE>(source_value, result, parameters, width,
		                                                                     scale);
	default:
		InternalException::Throw("Unimplemented internal type for decimal");
	}
}

//! DECIMAL -> VARCHAR
template <>
bool CastDecimalCInternal(duckdb_result *source, duckdb_string &result, idx_t col, idx_t row);

//! DECIMAL -> DECIMAL (internal fetch)
template <>
bool CastDecimalCInternal(duckdb_result *source, duckdb_decimal &result, idx_t col, idx_t row);

//! DECIMAL -> ...
template <class RESULT_TYPE>
RESULT_TYPE TryCastDecimalCInternal(duckdb_result *source, idx_t col, idx_t row) {
	RESULT_TYPE result_value;
	try {
		if (!CastDecimalCInternal<RESULT_TYPE>(source, result_value, col, row)) {
			return FetchDefaultValue::Operation<RESULT_TYPE>();
		}
	} catch (...) {
		return FetchDefaultValue::Operation<RESULT_TYPE>();
	}
	return result_value;
}

} // namespace duckdb
