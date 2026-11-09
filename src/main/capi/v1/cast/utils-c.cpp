#include "duckdb/main/capi/cast/utils.hpp"

namespace duckdb {

template <>
duckdb_decimal FetchDefaultValue::Operation() {
	duckdb_decimal result;
	result.scale = 0;
	result.width = 0;
	result.value = {0, 0};
	return result;
}

template <>
date_t FetchDefaultValue::Operation() {
	date_t result;
	result.days = 0;
	return result;
}

template <>
dtime_t FetchDefaultValue::Operation() {
	dtime_t result;
	result.value = 0;
	return result;
}

template <>
timestamp_t FetchDefaultValue::Operation() {
	timestamp_t result;
	result.value = 0;
	return result;
}

template <>
interval_t FetchDefaultValue::Operation() {
	interval_t result;
	result.months = 0;
	result.days = 0;
	result.micros = 0;
	return result;
}

template <>
char *FetchDefaultValue::Operation() {
	return nullptr;
}

template <>
duckdb_string FetchDefaultValue::Operation() {
	duckdb_string result;
	result.data = nullptr;
	result.size = 0;
	return result;
}

template <>
duckdb_blob FetchDefaultValue::Operation() {
	duckdb_blob result;
	result.data = nullptr;
	result.size = 0;
	return result;
}

//===--------------------------------------------------------------------===//
// Blob Casts
//===--------------------------------------------------------------------===//

template <>
bool FromCBlobCastWrapper::Operation(duckdb_blob input, duckdb_string &result) {
	string_t input_str(const_char_ptr_cast(input.data), UnsafeNumericCast<uint32_t>(input.size));
	return ToCStringCastWrapper<duckdb::CastFromBlob>::template Operation<string_t, duckdb_string>(input_str, result);
}

} // namespace duckdb

bool CanUseDeprecatedFetch(duckdb_result *result, idx_t col, idx_t row) {
	if (!result) {
		return false;
	}
	if (!duckdb::DeprecatedMaterializeResult(result)) {
		return false;
	}
	if (col >= result->deprecated_column_count || row >= result->deprecated_row_count) {
		return false;
	}
	if (!result->deprecated_columns[col].deprecated_nullmask) {
		// the column was not translated, e.g. because an earlier column failed to materialize
		return false;
	}
	return true;
}

bool CanFetchValue(duckdb_result *result, idx_t col, idx_t row) {
	if (!CanUseDeprecatedFetch(result, col, row)) {
		return false;
	}
	if (result->deprecated_columns[col].deprecated_nullmask[row]) {
		return false;
	}
	return true;
}
