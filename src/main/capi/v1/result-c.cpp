#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

struct CBaseConverter {
	template <class DST>
	static void NullConvert(DST &target) {
	}
};

struct CStandardConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		return input;
	}
};

struct CStringConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		auto result = char_ptr_cast(duckdb_malloc(input.GetSize() + 1));
		if (!result) {
			return nullptr;
		}
		memcpy((void *)result, input.GetData(), input.GetSize());
		auto write_arr = char_ptr_cast(result);
		write_arr[input.GetSize()] = '\0';
		return result;
	}

	template <class DST>
	static void NullConvert(DST &target) {
		target = nullptr;
	}
};

struct CBlobConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		duckdb_blob result;
		result.data = char_ptr_cast(duckdb_malloc(input.GetSize()));
		result.size = input.GetSize();
		if (!result.data) {
			result.size = 0;
			return result;
		}
		memcpy(result.data, input.GetData(), input.GetSize());
		return result;
	}

	template <class DST>
	static void NullConvert(DST &target) {
		target.data = nullptr;
		target.size = 0;
	}
};

struct CTimestampMsConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		if (!input.IsFinite()) {
			return input;
		}
		return Timestamp::FromEpochMs(input.value);
	}
};

struct CTimestampNsConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		if (!input.IsFinite()) {
			return input;
		}
		return Timestamp::FromEpochNanoSeconds(input.value);
	}
};

struct CTimestampSecConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		if (!input.IsFinite()) {
			return input;
		}
		return Timestamp::FromEpochSeconds(input.value);
	}
};

struct CHugeintConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		duckdb_hugeint result;
		result.lower = input.lower;
		result.upper = input.upper;
		return result;
	}
};

struct CUhugeintConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		duckdb_uhugeint result;
		result.lower = input.lower;
		result.upper = input.upper;
		return result;
	}
};

struct CIntervalConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		duckdb_interval result;
		result.days = input.days;
		result.months = input.months;
		result.micros = input.micros;
		return result;
	}
};

template <class T>
struct CDecimalConverter : public CBaseConverter {
	template <class SRC, class DST>
	static DST Convert(SRC input) {
		duckdb_hugeint result;
		result.lower = static_cast<uint64_t>(input);
		result.upper = 0;
		return result;
	}
};

template <class SRC, class DST = SRC, class OP = CStandardConverter>
void WriteData(duckdb_column *column, ColumnDataCollection &source, const vector<column_t> &column_ids) {
	idx_t row = 0;
	auto target = (DST *)column->deprecated_data;
	for (auto &input : source.Chunks(column_ids)) {
		// scans can return constant and dictionary vectors - read the cells in place
		auto source_values = input.data[0].Values<SRC>();
		for (auto entry : source_values) {
			if (!entry.IsValid()) {
				OP::template NullConvert<DST>(target[row++]);
			} else {
				target[row++] = OP::template Convert<SRC, DST>(entry.GetValueUnsafe());
			}
		}
	}
}

duckdb_state deprecated_duckdb_translate_column(QueryResult &result, duckdb_column *column, idx_t col) {
	D_ASSERT(!result.HasError());
	auto &collection = result.Collection();
	idx_t row_count = collection.Count();
	column->deprecated_nullmask = (bool *)duckdb_malloc(sizeof(bool) * collection.Count());
	if (!column->deprecated_nullmask) {
		return DuckDBError;
	}

	auto type_size = GetCTypeSize(column->deprecated_type);
	if (type_size == 0) {
		for (idx_t row_id = 0; row_id < row_count; row_id++) {
			column->deprecated_nullmask[row_id] = false;
		}
		// Unsupported type, e.g., a LIST. By returning DuckDBSuccess here,
		// we allow filling other columns, and return NULL for all unsupported types.
		return DuckDBSuccess;
	}

	column->deprecated_data = duckdb_malloc(type_size * row_count);
	if (!column->deprecated_data) { // LCOV_EXCL_START
		// malloc failure
		return DuckDBError;
	} // LCOV_EXCL_STOP

	vector<column_t> column_ids {col};
	// first convert the nullmask
	{
		idx_t row = 0;
		for (auto &input : collection.Chunks(column_ids)) {
			// scans can return constant and dictionary vectors - read the validity in place
			auto validity = input.data[0].Validity();
			for (idx_t k = 0; k < input.size(); k++) {
				column->deprecated_nullmask[row++] = !validity.IsValid(k);
			}
		}
	}
	// then write the data
	switch (result.GetTypes()[col].id()) {
	case LogicalTypeId::SQLNULL:
		// every value of a SQLNULL column is NULL, which the nullmask already reflects - zero the data
		memset(column->deprecated_data, 0, type_size * row_count);
		break;
	case LogicalTypeId::BOOLEAN:
		WriteData<bool>(column, collection, column_ids);
		break;
	case LogicalTypeId::TINYINT:
		WriteData<int8_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::SMALLINT:
		WriteData<int16_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::INTEGER:
		WriteData<int32_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::BIGINT:
		WriteData<int64_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::UTINYINT:
		WriteData<uint8_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::USMALLINT:
		WriteData<uint16_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::UINTEGER:
		WriteData<uint32_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::UBIGINT:
		WriteData<uint64_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::FLOAT:
		WriteData<float>(column, collection, column_ids);
		break;
	case LogicalTypeId::DOUBLE:
		WriteData<double>(column, collection, column_ids);
		break;
	case LogicalTypeId::DATE:
		WriteData<date_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::TIME:
		WriteData<dtime_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::TIME_NS:
		WriteData<dtime_ns_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::TIME_TZ:
		WriteData<dtime_tz_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		WriteData<timestamp_t>(column, collection, column_ids);
		break;
	case LogicalTypeId::VARCHAR: {
		WriteData<string_t, const char *, CStringConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::BLOB: {
		WriteData<string_t, duckdb_blob, CBlobConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		WriteData<timestamp_t, timestamp_t, CTimestampNsConverter>(column, collection, column_ids);
		break;
	case LogicalTypeId::TIMESTAMP_MS: {
		WriteData<timestamp_t, timestamp_t, CTimestampMsConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::TIMESTAMP_SEC: {
		WriteData<timestamp_t, timestamp_t, CTimestampSecConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::HUGEINT: {
		WriteData<hugeint_t, duckdb_hugeint, CHugeintConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::UHUGEINT: {
		WriteData<uhugeint_t, duckdb_uhugeint, CUhugeintConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::INTERVAL: {
		WriteData<interval_t, duckdb_interval, CIntervalConverter>(column, collection, column_ids);
		break;
	}
	case LogicalTypeId::DECIMAL: {
		// get data
		switch (result.GetTypes()[col].InternalType()) {
		case PhysicalType::INT16: {
			WriteData<int16_t, duckdb_hugeint, CDecimalConverter<int16_t>>(column, collection, column_ids);
			break;
		}
		case PhysicalType::INT32: {
			WriteData<int32_t, duckdb_hugeint, CDecimalConverter<int32_t>>(column, collection, column_ids);
			break;
		}
		case PhysicalType::INT64: {
			WriteData<int64_t, duckdb_hugeint, CDecimalConverter<int64_t>>(column, collection, column_ids);
			break;
		}
		case PhysicalType::INT128: {
			WriteData<hugeint_t, duckdb_hugeint, CHugeintConverter>(column, collection, column_ids);
			break;
		}
		default:
			throw std::runtime_error("Unsupported physical type for Decimal" +
			                         TypeIdToString(result.GetTypes()[col].InternalType()));
		}
		break;
	}
	default: // LCOV_EXCL_START
		return DuckDBError;
	} // LCOV_EXCL_STOP
	return DuckDBSuccess;
}

duckdb_state DuckDBTranslateStreamResult(unique_ptr<QueryResultStream<>> stream_p, duckdb_result *out) {
	D_ASSERT(stream_p);
	auto &stream = *stream_p;
	if (!out) {
		// no result to write to, only return the status
		return !stream.HasError() ? DuckDBSuccess : DuckDBError;
	}
	memset(out, 0, sizeof(duckdb_result));

	auto result_data = new DuckDBResultData();
	result_data->stream = std::move(stream_p);
	result_data->result_set_type = CAPIResultSetType::CAPI_RESULT_TYPE_NONE;
	out->internal_data = result_data;

	if (stream.HasError()) {
		out->deprecated_error_message = (char *)stream.GetError().c_str(); // NOLINT
		return DuckDBError;
	}
	out->deprecated_column_count = stream.ColumnCount();
	out->deprecated_rows_changed = 0;
	return DuckDBSuccess;
}

duckdb_state DuckDBTranslateResult(unique_ptr<QueryResult> result_p, duckdb_result *out) {
	auto &result = *result_p;
	D_ASSERT(result_p);
	if (!out) {
		// no result to write to, only return the status
		return !result.HasError() ? DuckDBSuccess : DuckDBError;
	}

	memset(out, 0, sizeof(duckdb_result));

	// initialize the result_data object
	auto result_data = new DuckDBResultData();
	result_data->result = std::move(result_p);
	result_data->result_set_type = CAPIResultSetType::CAPI_RESULT_TYPE_NONE;
	out->internal_data = result_data;

	if (result.HasError()) {
		// write the error message
		out->deprecated_error_message = (char *)result.GetError().c_str(); // NOLINT
		return DuckDBError;
	}
	// copy the data
	// first write the metadata
	out->deprecated_column_count = result.ColumnCount();
	out->deprecated_rows_changed = 0;
	return DuckDBSuccess;
}

//! Drain a stream into a retained result, for the random-access deprecated result set
static unique_ptr<QueryResult> DrainStreamIntoResult(QueryResultStream<> &stream) {
	auto collection = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), stream.GetTypes());
	ColumnDataAppendState append_state;
	collection->InitializeAppend(append_state);
	while (auto chunk = stream.Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		collection->Append(append_state, *chunk);
	}
	if (stream.HasError()) {
		return make_uniq<QueryResult>(stream.GetErrorObject());
	}
	return make_uniq<QueryResult>(stream.GetStatementType(), stream.GetStatementProperties(), stream.GetNames(),
	                              std::move(collection), stream.GetClientProperties());
}

bool DeprecatedMaterializeResult(duckdb_result *result) {
	if (!result) {
		return false;
	}
	auto result_data = reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data);
	if (result_data->HasError()) {
		return false;
	}
	if (result_data->result_set_type == CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED) {
		// already materialized into deprecated result format
		return true;
	}
	if (result_data->result_set_type == CAPIResultSetType::CAPI_RESULT_TYPE_MATERIALIZED) {
		// already used as a new result set
		return false;
	}
	if (result_data->result_set_type == CAPIResultSetType::CAPI_RESULT_TYPE_STREAMING) {
		// already used as a streaming result
		return false;
	}
	// materialize as deprecated result set
	result_data->result_set_type = CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED;
	auto column_count = result_data->ColumnCount();
	result->deprecated_columns = (duckdb_column *)duckdb_malloc(sizeof(duckdb_column) * column_count);
	if (!result->deprecated_columns) { // LCOV_EXCL_START
		// malloc failure
		return DuckDBError;
	} // LCOV_EXCL_STOP

	if (result_data->IsStreaming()) {
		// The deprecated result set is random-access: drain the stream into a collection
		result_data->result = DrainStreamIntoResult(*result_data->stream);
		result_data->stream.reset();
		if (result_data->result->HasError()) {
			return false;
		}
	}
	auto &materialized = *result_data->result;

	// convert the result to a materialized result
	// zero initialize the columns (so we can cleanly delete it in case a malloc fails)
	memset(result->deprecated_columns, 0, sizeof(duckdb_column) * column_count);
	for (idx_t i = 0; i < column_count; i++) {
		result->deprecated_columns[i].deprecated_type = LogicalTypeIdToC(materialized.GetTypes()[i].id());
		result->deprecated_columns[i].deprecated_name = (char *)materialized.ColumnName(i).c_str(); // NOLINT
	}

	result->deprecated_row_count = materialized.RowCount();
	if (result->deprecated_row_count > 0 &&
	    materialized.GetStatementProperties().return_type == StatementReturnType::CHANGED_ROWS) {
		// update total changes
		auto row_changes = materialized.Collection().GetValue(0, 0);
		if (!row_changes.IsNull()) {
			auto cast_row_changes = row_changes.DefaultTryCastAs(LogicalType::BIGINT);
			if (cast_row_changes) {
				result->deprecated_rows_changed = NumericCast<idx_t>(cast_row_changes->GetValue<int64_t>());
			}
		}
	}

	// Now write the data and skip any unsupported columns.
	for (idx_t col = 0; col < column_count; col++) {
		auto state = deprecated_duckdb_translate_column(materialized, &result->deprecated_columns[col], col);
		if (state != DuckDBSuccess) {
			return false;
		}
	}
	return true;
}

} // namespace duckdb

static void DuckdbDestroyColumn(duckdb_column column, idx_t count) {
	if (column.deprecated_data) {
		if (column.deprecated_type == DUCKDB_TYPE_VARCHAR) {
			// varchar, delete individual strings
			auto data = reinterpret_cast<char **>(column.deprecated_data);
			for (idx_t i = 0; i < count; i++) {
				if (data[i]) {
					duckdb_free(data[i]);
				}
			}
		} else if (column.deprecated_type == DUCKDB_TYPE_BLOB) {
			// blob, delete individual blobs
			auto data = reinterpret_cast<duckdb_blob *>(column.deprecated_data);
			for (idx_t i = 0; i < count; i++) {
				if (data[i].data) {
					duckdb_free((void *)data[i].data);
				}
			}
		}
		duckdb_free(column.deprecated_data);
	}
	if (column.deprecated_nullmask) {
		duckdb_free(column.deprecated_nullmask);
	}
}

void duckdb_destroy_result(duckdb_result *result) {
	if (result->deprecated_columns) {
		for (idx_t i = 0; i < result->deprecated_column_count; i++) {
			DuckdbDestroyColumn(result->deprecated_columns[i], result->deprecated_row_count);
		}
		duckdb_free(result->deprecated_columns);
	}
	if (result->internal_data) {
		auto result_data = reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data);
		delete result_data;
	}
	memset(result, 0, sizeof(duckdb_result));
}

const char *duckdb_column_name(duckdb_result *result, idx_t col) {
	if (!result || col >= duckdb_column_count(result)) {
		return nullptr;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	return result_data.ColumnName(col).c_str();
}

duckdb_type duckdb_column_type(duckdb_result *result, idx_t col) {
	if (!result || col >= duckdb_column_count(result)) {
		return DUCKDB_TYPE_INVALID;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	return duckdb::LogicalTypeIdToC(result_data.GetTypes()[col].id());
}

duckdb_logical_type duckdb_column_logical_type(duckdb_result *result, idx_t col) {
	if (!result || col >= duckdb_column_count(result)) {
		return nullptr;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(result_data.GetTypes()[col]));
}

duckdb_arrow_options duckdb_result_get_arrow_options(duckdb_result *result) {
	if (!result) {
		return nullptr;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	if (!result_data.result && !result_data.stream) {
		return nullptr;
	}
	auto arrow_options_wrapper = new duckdb::CClientArrowOptionsWrapper(result_data.GetClientProperties());
	return reinterpret_cast<duckdb_arrow_options>(arrow_options_wrapper);
}

idx_t duckdb_column_count(duckdb_result *result) {
	if (!result) {
		return 0;
	}
	if (result->internal_data == NULL) {
		return 0;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	return result_data.ColumnCount();
}

idx_t duckdb_row_count(duckdb_result *result) {
	if (!result) {
		return 0;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	if (result_data.IsStreaming()) {
		// We can't know the row count beforehand
		return 0;
	}
	return result_data.Retained().RowCount();
}

idx_t duckdb_rows_changed(duckdb_result *result) {
	if (!result) {
		return 0;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	if (result_data.result_set_type == duckdb::CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED) {
		// not a materialized result
		return result->deprecated_rows_changed;
	}
	if (result_data.IsStreaming()) {
		return 0;
	}
	auto &materialized = result_data.Retained();
	if (materialized.GetStatementProperties().return_type != duckdb::StatementReturnType::CHANGED_ROWS) {
		// we can only use this function for CHANGED_ROWS result types
		return 0;
	}
	if (materialized.RowCount() != 1 || materialized.ColumnCount() != 1) {
		// CHANGED_ROWS should return exactly one row
		return 0;
	}
	return materialized.Collection().GetValue(0, 0).GetValue<uint64_t>();
}

void *duckdb_column_data(duckdb_result *result, idx_t col) {
	if (!result || col >= result->deprecated_column_count) {
		return nullptr;
	}
	if (!duckdb::DeprecatedMaterializeResult(result)) {
		return nullptr;
	}
	return result->deprecated_columns[col].deprecated_data;
}

bool *duckdb_nullmask_data(duckdb_result *result, idx_t col) {
	if (!result || col >= result->deprecated_column_count) {
		return nullptr;
	}
	if (!duckdb::DeprecatedMaterializeResult(result)) {
		return nullptr;
	}
	return result->deprecated_columns[col].deprecated_nullmask;
}

const char *duckdb_result_error(duckdb_result *result) {
	if (!result || !result->internal_data) {
		return nullptr;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	return !result_data.HasError() ? nullptr : result_data.GetError().c_str();
}

duckdb_error_type duckdb_result_error_type(duckdb_result *result) {
	if (!result || !result->internal_data) {
		return DUCKDB_ERROR_INVALID;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result->internal_data));
	if (!result_data.HasError()) {
		return DUCKDB_ERROR_INVALID;
	}
	return duckdb::ErrorTypeToC(result_data.GetErrorType());
}

idx_t duckdb_result_chunk_count(duckdb_result result) {
	if (!result.internal_data) {
		return 0;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result.internal_data));
	if (result_data.result_set_type == duckdb::CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED) {
		return 0;
	}
	if (result_data.IsStreaming()) {
		// Can't know beforehand how many chunks are returned.
		return 0;
	}
	return result_data.Retained().Collection().ChunkCount();
}

duckdb_data_chunk duckdb_result_get_chunk(duckdb_result result, idx_t chunk_idx) {
	if (!result.internal_data) {
		return nullptr;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result.internal_data));
	if (result_data.result_set_type == duckdb::CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED) {
		return nullptr;
	}
	if (result_data.IsStreaming()) {
		// This API is only supported for retained query results
		return nullptr;
	}
	result_data.result_set_type = duckdb::CAPIResultSetType::CAPI_RESULT_TYPE_MATERIALIZED;
	auto &collection = result_data.Retained().Collection();
	if (chunk_idx >= collection.ChunkCount()) {
		return nullptr;
	}
	try {
		auto chunk = duckdb::make_uniq<duckdb::DataChunk>();
		chunk->Initialize(duckdb::Allocator::DefaultAllocator(), collection.Types());
		collection.FetchChunk(chunk_idx, *chunk);
		for (auto &vector : chunk->data) {
			duckdb::PrepareCAPIVector(vector);
		}
		return reinterpret_cast<duckdb_data_chunk>(chunk.release());
	} catch (std::exception &e) {
		result_data.result->SetError(duckdb::ErrorData(e));
		return nullptr;
	}
}

bool duckdb_result_is_streaming(duckdb_result result) {
	if (!result.internal_data) {
		return false;
	}
	if (duckdb_result_error(&result) != nullptr) {
		return false;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result.internal_data));
	return result_data.IsStreaming();
}

duckdb_result_type duckdb_result_return_type(duckdb_result result) {
	if (!result.internal_data || duckdb_result_error(&result) != nullptr) {
		return DUCKDB_RESULT_TYPE_INVALID;
	}
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result.internal_data));
	switch (result_data.GetStatementProperties().return_type) {
	case duckdb::StatementReturnType::CHANGED_ROWS:
		return DUCKDB_RESULT_TYPE_CHANGED_ROWS;
	case duckdb::StatementReturnType::NOTHING:
		return DUCKDB_RESULT_TYPE_NOTHING;
	case duckdb::StatementReturnType::QUERY_RESULT:
		return DUCKDB_RESULT_TYPE_QUERY_RESULT;
	default:
		return DUCKDB_RESULT_TYPE_INVALID;
	}
}

duckdb_statement_type duckdb_result_statement_type(duckdb_result result) {
	if (!result.internal_data || duckdb_result_error(&result) != nullptr) {
		return DUCKDB_STATEMENT_TYPE_INVALID;
	}
	auto &pres = *(reinterpret_cast<duckdb::DuckDBResultData *>(result.internal_data));

	return StatementTypeToC(pres.GetStatementType());
}
