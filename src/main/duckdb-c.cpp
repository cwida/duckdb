#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/arrow.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/appender.hpp"

#include "duckdb.h"
#include "duckdb.hpp"

#include <cstring>
#include <cassert>

#ifdef _WIN32
#define strdup _strdup
#endif

#ifdef GetCValue
#undef GetCValue
#endif

using namespace duckdb;

static duckdb_type ConvertCPPTypeToC(LogicalType type);
static idx_t GetCTypeSize(duckdb_type type);
namespace duckdb {
struct DatabaseData {
	DatabaseData() : database(nullptr) {
	}
	~DatabaseData() {
		if (database) {
			delete database;
		}
	}

	DuckDB *database;
};
} // namespace duckdb
duckdb_state duckdb_open(const char *path, duckdb_database *out) {
	auto wrapper = new DatabaseData();
	try {
		wrapper->database = new DuckDB(path);
	} catch (...) {
		delete wrapper;
		return DuckDBError;
	}
	*out = (duckdb_database)wrapper;
	return DuckDBSuccess;
}

void duckdb_close(duckdb_database *database) {
	if (*database) {
		auto wrapper = (DatabaseData *)*database;
		delete wrapper;
		*database = nullptr;
	}
}

duckdb_state duckdb_connect(duckdb_database database, duckdb_connection *out) {
	auto wrapper = (DatabaseData *)database;
	Connection *connection;
	try {
		connection = new Connection(*wrapper->database);
	} catch (...) {
		return DuckDBError;
	}
	*out = (duckdb_connection)connection;
	return DuckDBSuccess;
}

void duckdb_disconnect(duckdb_connection *connection) {
	if (*connection) {
		Connection *conn = (Connection *)*connection;
		delete conn;
		*connection = nullptr;
	}
}

template <class T>
void WriteData(duckdb_result *out, ChunkCollection &source, idx_t col) {
	idx_t row = 0;
	auto target = (T *)out->columns[col].data;
	for (auto &chunk : source.Chunks()) {
		auto source = FlatVector::GetData<T>(chunk->data[col]);
		auto &mask = FlatVector::Validity(chunk->data[col]);

		for (idx_t k = 0; k < chunk->size(); k++, row++) {
			if (!mask.RowIsValid(k)) {
				continue;
			}
			target[row] = source[k];
		}
	}
}

static duckdb_state duckdb_translate_result(MaterializedQueryResult *result, duckdb_result *out) {
	D_ASSERT(result);
	if (!out) {
		// no result to write to, only return the status
		return result->success ? DuckDBSuccess : DuckDBError;
	}
	out->error_message = nullptr;
	if (!result->success) {
		// write the error message
		out->error_message = strdup(result->error.c_str());
		return DuckDBError;
	}
	// copy the data
	// first write the meta data
	out->column_count = result->types.size();
	out->row_count = result->collection.Count();
	out->rows_changed = 0;
	if (out->row_count > 0 && StatementTypeReturnChanges(result->statement_type)) {
		// update total changes
		auto row_changes = result->GetValue(0, 0);
		if (!row_changes.is_null && row_changes.TryCastAs(LogicalType::BIGINT)) {
			out->rows_changed = row_changes.GetValue<int64_t>();
		}
	}
	out->columns = (duckdb_column *)malloc(sizeof(duckdb_column) * out->column_count);
	if (!out->columns) { // LCOV_EXCL_START
		// malloc failure
		return DuckDBError;
	} // LCOV_EXCL_STOP

	// zero initialize the columns (so we can cleanly delete it in case a malloc fails)
	memset(out->columns, 0, sizeof(duckdb_column) * out->column_count);
	for (idx_t i = 0; i < out->column_count; i++) {
		out->columns[i].type = ConvertCPPTypeToC(result->types[i]);
		out->columns[i].name = strdup(result->names[i].c_str());
		out->columns[i].nullmask = (bool *)malloc(sizeof(bool) * out->row_count);
		out->columns[i].data = malloc(GetCTypeSize(out->columns[i].type) * out->row_count);
		if (!out->columns[i].nullmask || !out->columns[i].name || !out->columns[i].data) { // LCOV_EXCL_START
			// malloc failure
			return DuckDBError;
		} // LCOV_EXCL_STOP
		// memset data to 0 for VARCHAR columns for safe deletion later
		if (result->types[i].InternalType() == PhysicalType::VARCHAR) {
			memset(out->columns[i].data, 0, GetCTypeSize(out->columns[i].type) * out->row_count);
		}
	}
	// now write the data
	for (idx_t col = 0; col < out->column_count; col++) {
		// first set the nullmask
		idx_t row = 0;
		for (auto &chunk : result->collection.Chunks()) {
			for (idx_t k = 0; k < chunk->size(); k++) {
				out->columns[col].nullmask[row++] = FlatVector::IsNull(chunk->data[col], k);
			}
		}
		// then write the data
		switch (result->types[col].id()) {
		case LogicalTypeId::BOOLEAN:
			WriteData<bool>(out, result->collection, col);
			break;
		case LogicalTypeId::TINYINT:
			WriteData<int8_t>(out, result->collection, col);
			break;
		case LogicalTypeId::SMALLINT:
			WriteData<int16_t>(out, result->collection, col);
			break;
		case LogicalTypeId::INTEGER:
			WriteData<int32_t>(out, result->collection, col);
			break;
		case LogicalTypeId::BIGINT:
			WriteData<int64_t>(out, result->collection, col);
			break;
		case LogicalTypeId::UTINYINT:
			WriteData<uint8_t>(out, result->collection, col);
			break;
		case LogicalTypeId::USMALLINT:
			WriteData<uint16_t>(out, result->collection, col);
			break;
		case LogicalTypeId::UINTEGER:
			WriteData<uint32_t>(out, result->collection, col);
			break;
		case LogicalTypeId::UBIGINT:
			WriteData<uint64_t>(out, result->collection, col);
			break;
		case LogicalTypeId::FLOAT:
			WriteData<float>(out, result->collection, col);
			break;
		case LogicalTypeId::DOUBLE:
			WriteData<double>(out, result->collection, col);
			break;
		case LogicalTypeId::VARCHAR: {
			idx_t row = 0;
			auto target = (const char **)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<string_t>(chunk->data[col]);
				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						target[row] = (char *)malloc(source[k].GetSize() + 1);
						assert(target[row]);
						memcpy((void *)target[row], source[k].GetDataUnsafe(), source[k].GetSize());
						auto write_arr = (char *)target[row];
						write_arr[source[k].GetSize()] = '\0';
					}
					row++;
				}
			}
			break;
		}
		case LogicalTypeId::BLOB: {
			idx_t row = 0;
			auto target = (duckdb_blob *)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<string_t>(chunk->data[col]);
				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						target[row].data = (char *)malloc(source[k].GetSize());
						target[row].size = source[k].GetSize();
						assert(target[row].data);
						memcpy((void *)target[row].data, source[k].GetDataUnsafe(), source[k].GetSize());
					}
					row++;
				}
			}
			break;
		}
		case LogicalTypeId::DATE: {
			idx_t row = 0;
			auto target = (duckdb_date *)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<date_t>(chunk->data[col]);
				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						int32_t year, month, day;
						Date::Convert(source[k], year, month, day);
						target[row].year = year;
						target[row].month = month;
						target[row].day = day;
					}
					row++;
				}
			}
			break;
		}
		case LogicalTypeId::TIME: {
			idx_t row = 0;
			auto target = (duckdb_time *)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<dtime_t>(chunk->data[col]);
				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						int32_t hour, min, sec, micros;
						Time::Convert(source[k], hour, min, sec, micros);
						target[row].hour = hour;
						target[row].min = min;
						target[row].sec = sec;
						target[row].micros = micros;
					}
					row++;
				}
			}
			break;
		}
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC: {
			idx_t row = 0;
			auto target = (duckdb_timestamp *)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<timestamp_t>(chunk->data[col]);

				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						date_t date;
						dtime_t time;
						auto source_value = source[k];
						if (result->types[col].id() == LogicalTypeId::TIMESTAMP_NS) {
							source_value = Timestamp::FromEpochNanoSeconds(source[k].value);
						} else if (result->types[col].id() == LogicalTypeId::TIMESTAMP_MS) {
							source_value = Timestamp::FromEpochMs(source[k].value);
						} else if (result->types[col].id() == LogicalTypeId::TIMESTAMP_SEC) {
							source_value = Timestamp::FromEpochSeconds(source[k].value);
						}
						Timestamp::Convert(source_value, date, time);

						int32_t year, month, day;
						Date::Convert(date, year, month, day);

						int32_t hour, min, sec, micros;
						Time::Convert(time, hour, min, sec, micros);

						target[row].date.year = year;
						target[row].date.month = month;
						target[row].date.day = day;
						target[row].time.hour = hour;
						target[row].time.min = min;
						target[row].time.sec = sec;
						target[row].time.micros = micros;
					}
					row++;
				}
			}
			break;
		}
		case LogicalTypeId::HUGEINT: {
			idx_t row = 0;
			auto target = (duckdb_hugeint *)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<hugeint_t>(chunk->data[col]);
				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						target[row].lower = source[k].lower;
						target[row].upper = source[k].upper;
					}
					row++;
				}
			}
			break;
		}
		case LogicalTypeId::INTERVAL: {
			idx_t row = 0;
			auto target = (duckdb_interval *)out->columns[col].data;
			for (auto &chunk : result->collection.Chunks()) {
				auto source = FlatVector::GetData<interval_t>(chunk->data[col]);
				for (idx_t k = 0; k < chunk->size(); k++) {
					if (!FlatVector::IsNull(chunk->data[col], k)) {
						target[row].days = source[k].days;
						target[row].months = source[k].months;
						target[row].micros = source[k].micros;
					}
					row++;
				}
			}
			break;
		}
		default:
			// unsupported type for C API
			D_ASSERT(0);
			return DuckDBError;
		}
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_query(duckdb_connection connection, const char *query, duckdb_result *out) {
	Connection *conn = (Connection *)connection;
	auto result = conn->Query(query);
	return duckdb_translate_result(result.get(), out);
}

namespace duckdb {
struct ArrowResultWrapper {
	ArrowResultWrapper() : result(nullptr), current_chunk(nullptr) {
	}
	~ArrowResultWrapper() {
	}
	unique_ptr<MaterializedQueryResult> result;
	unique_ptr<DataChunk> current_chunk;
};
} // namespace duckdb

duckdb_state duckdb_query_arrow(duckdb_connection connection, const char *query, duckdb_arrow *out_result) {
	Connection *conn = (Connection *)connection;
	auto wrapper = new ArrowResultWrapper();
	wrapper->result = conn->Query(query);
	*out_result = (duckdb_arrow)wrapper;
	return wrapper->result->success ? DuckDBSuccess : DuckDBError;
}

duckdb_state duckdb_query_arrow_schema(duckdb_arrow result, duckdb_arrow_schema *out_schema) {
	if (!out_schema) {
		return DuckDBSuccess;
	}
	auto wrapper = (ArrowResultWrapper *)result;
	wrapper->result->ToArrowSchema((ArrowSchema *)*out_schema);
	return DuckDBSuccess;
}

duckdb_state duckdb_query_arrow_array(duckdb_arrow result, duckdb_arrow_array *out_array) {
	if (!out_array) {
		return DuckDBSuccess;
	}
	auto wrapper = (ArrowResultWrapper *)result;
	auto success = wrapper->result->TryFetch(wrapper->current_chunk, wrapper->result->error);
	if (!success) {
		return DuckDBError;
	}
	if (!wrapper->current_chunk || wrapper->current_chunk->size() == 0) {
		return DuckDBSuccess;
	}
	wrapper->current_chunk->ToArrowArray((ArrowArray *)*out_array);
	return DuckDBSuccess;
}

const char *duckdb_query_arrow_error(duckdb_arrow result) {
	auto wrapper = (ArrowResultWrapper *)result;
	return wrapper->result->error.c_str();
}

void duckdb_destroy_arrow(duckdb_arrow *result) {
	if (*result) {
		auto wrapper = (ArrowResultWrapper *)*result;
		delete wrapper;
		*result = nullptr;
	}
}

static void duckdb_destroy_column(duckdb_column column, idx_t count) {
	if (column.data) {
		if (column.type == DUCKDB_TYPE_VARCHAR) {
			// varchar, delete individual strings
			auto data = (char **)column.data;
			for (idx_t i = 0; i < count; i++) {
				if (data[i]) {
					free(data[i]);
				}
			}
		} else if (column.type == DUCKDB_TYPE_BLOB) {
			// blob, delete individual blobs
			auto data = (duckdb_blob *)column.data;
			for (idx_t i = 0; i < count; i++) {
				if (data[i].data) {
					free((void *)data[i].data);
				}
			}
		}
		free(column.data);
	}
	if (column.nullmask) {
		free(column.nullmask);
	}
	if (column.name) {
		free(column.name);
	}
}

void duckdb_destroy_result(duckdb_result *result) {
	if (result->error_message) {
		free(result->error_message);
	}
	if (result->columns) {
		for (idx_t i = 0; i < result->column_count; i++) {
			duckdb_destroy_column(result->columns[i], result->row_count);
		}
		free(result->columns);
	}
	memset(result, 0, sizeof(duckdb_result));
}
namespace duckdb {
struct PreparedStatementWrapper {
	PreparedStatementWrapper() : statement(nullptr) {
	}
	~PreparedStatementWrapper() {
	}
	unique_ptr<PreparedStatement> statement;
	vector<Value> values;
};
} // namespace duckdb
duckdb_state duckdb_prepare(duckdb_connection connection, const char *query,
                            duckdb_prepared_statement *out_prepared_statement) {
	if (!connection || !query) {
		return DuckDBError;
	}
	auto wrapper = new PreparedStatementWrapper();
	Connection *conn = (Connection *)connection;
	wrapper->statement = conn->Prepare(query);
	*out_prepared_statement = (duckdb_prepared_statement)wrapper;
	return wrapper->statement->success ? DuckDBSuccess : DuckDBError;
}

const char *duckdb_prepare_error(duckdb_prepared_statement prepared_statement) {
	auto wrapper = (PreparedStatementWrapper *)prepared_statement;
	if (!wrapper || !wrapper->statement || wrapper->statement->success) {
		return nullptr;
	}
	return wrapper->statement->error.c_str();
}

duckdb_state duckdb_nparams(duckdb_prepared_statement prepared_statement, idx_t *nparams_out) {
	auto wrapper = (PreparedStatementWrapper *)prepared_statement;
	if (!wrapper || !wrapper->statement || !wrapper->statement->success) {
		return DuckDBError;
	}
	*nparams_out = wrapper->statement->n_param;
	return DuckDBSuccess;
}

static duckdb_state duckdb_bind_value(duckdb_prepared_statement prepared_statement, idx_t param_idx, Value val) {
	auto wrapper = (PreparedStatementWrapper *)prepared_statement;
	if (!wrapper || !wrapper->statement || !wrapper->statement->success) {
		return DuckDBError;
	}
	if (param_idx > wrapper->statement->n_param) {
		return DuckDBError;
	}
	if (param_idx > wrapper->values.size()) {
		wrapper->values.resize(param_idx);
	}
	wrapper->values[param_idx - 1] = val;
	return DuckDBSuccess;
}

duckdb_state duckdb_bind_boolean(duckdb_prepared_statement prepared_statement, idx_t param_idx, bool val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::BOOLEAN(val));
}

duckdb_state duckdb_bind_int8(duckdb_prepared_statement prepared_statement, idx_t param_idx, int8_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::TINYINT(val));
}

duckdb_state duckdb_bind_int16(duckdb_prepared_statement prepared_statement, idx_t param_idx, int16_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::SMALLINT(val));
}

duckdb_state duckdb_bind_int32(duckdb_prepared_statement prepared_statement, idx_t param_idx, int32_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::INTEGER(val));
}

duckdb_state duckdb_bind_int64(duckdb_prepared_statement prepared_statement, idx_t param_idx, int64_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::BIGINT(val));
}

duckdb_state duckdb_bind_uint8(duckdb_prepared_statement prepared_statement, idx_t param_idx, uint8_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::UTINYINT(val));
}

duckdb_state duckdb_bind_uint16(duckdb_prepared_statement prepared_statement, idx_t param_idx, uint16_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::USMALLINT(val));
}

duckdb_state duckdb_bind_uint32(duckdb_prepared_statement prepared_statement, idx_t param_idx, uint32_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::UINTEGER(val));
}

duckdb_state duckdb_bind_uint64(duckdb_prepared_statement prepared_statement, idx_t param_idx, uint64_t val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::UBIGINT(val));
}

duckdb_state duckdb_bind_float(duckdb_prepared_statement prepared_statement, idx_t param_idx, float val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value(val));
}

duckdb_state duckdb_bind_double(duckdb_prepared_statement prepared_statement, idx_t param_idx, double val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value(val));
}

duckdb_state duckdb_bind_varchar(duckdb_prepared_statement prepared_statement, idx_t param_idx, const char *val) {
	return duckdb_bind_value(prepared_statement, param_idx, Value(val));
}

duckdb_state duckdb_bind_varchar_length(duckdb_prepared_statement prepared_statement, idx_t param_idx, const char *val,
                                        idx_t length) {
	return duckdb_bind_value(prepared_statement, param_idx, Value(string(val, length)));
}

duckdb_state duckdb_bind_blob(duckdb_prepared_statement prepared_statement, idx_t param_idx, const void *data,
                              idx_t length) {
	return duckdb_bind_value(prepared_statement, param_idx, Value::BLOB((const_data_ptr_t)data, length));
}

duckdb_state duckdb_bind_null(duckdb_prepared_statement prepared_statement, idx_t param_idx) {
	return duckdb_bind_value(prepared_statement, param_idx, Value());
}

duckdb_state duckdb_execute_prepared(duckdb_prepared_statement prepared_statement, duckdb_result *out_result) {
	auto wrapper = (PreparedStatementWrapper *)prepared_statement;
	if (!wrapper || !wrapper->statement || !wrapper->statement->success) {
		return DuckDBError;
	}
	auto result = wrapper->statement->Execute(wrapper->values, false);
	D_ASSERT(result->type == QueryResultType::MATERIALIZED_RESULT);
	auto mat_res = (MaterializedQueryResult *)result.get();
	return duckdb_translate_result(mat_res, out_result);
}

duckdb_state duckdb_execute_prepared_arrow(duckdb_prepared_statement prepared_statement, duckdb_arrow *out_result) {
	auto wrapper = (PreparedStatementWrapper *)prepared_statement;
	if (!wrapper || !wrapper->statement || !wrapper->statement->success) {
		return DuckDBError;
	}
	auto arrow_wrapper = new ArrowResultWrapper();
	auto result = wrapper->statement->Execute(wrapper->values, false);
	D_ASSERT(result->type == QueryResultType::MATERIALIZED_RESULT);
	arrow_wrapper->result =
	    unique_ptr<MaterializedQueryResult>(static_cast<MaterializedQueryResult *>(result.release()));
	*out_result = (duckdb_arrow)arrow_wrapper;
	return arrow_wrapper->result->success ? DuckDBSuccess : DuckDBError;
}

void duckdb_destroy_prepare(duckdb_prepared_statement *prepared_statement) {
	if (!prepared_statement) {
		return;
	}
	auto wrapper = (PreparedStatementWrapper *)*prepared_statement;
	if (wrapper) {
		delete wrapper;
	}
	*prepared_statement = nullptr;
}

duckdb_type ConvertCPPTypeToC(LogicalType sql_type) {
	switch (sql_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return DUCKDB_TYPE_BOOLEAN;
	case LogicalTypeId::TINYINT:
		return DUCKDB_TYPE_TINYINT;
	case LogicalTypeId::SMALLINT:
		return DUCKDB_TYPE_SMALLINT;
	case LogicalTypeId::INTEGER:
		return DUCKDB_TYPE_INTEGER;
	case LogicalTypeId::BIGINT:
		return DUCKDB_TYPE_BIGINT;
	case LogicalTypeId::UTINYINT:
		return DUCKDB_TYPE_UTINYINT;
	case LogicalTypeId::USMALLINT:
		return DUCKDB_TYPE_USMALLINT;
	case LogicalTypeId::UINTEGER:
		return DUCKDB_TYPE_UINTEGER;
	case LogicalTypeId::UBIGINT:
		return DUCKDB_TYPE_UBIGINT;
	case LogicalTypeId::HUGEINT:
		return DUCKDB_TYPE_HUGEINT;
	case LogicalTypeId::FLOAT:
		return DUCKDB_TYPE_FLOAT;
	case LogicalTypeId::DOUBLE:
		return DUCKDB_TYPE_DOUBLE;
	case LogicalTypeId::TIMESTAMP:
		return DUCKDB_TYPE_TIMESTAMP;
	case LogicalTypeId::TIMESTAMP_SEC:
		return DUCKDB_TYPE_TIMESTAMP_S;
	case LogicalTypeId::TIMESTAMP_MS:
		return DUCKDB_TYPE_TIMESTAMP_MS;
	case LogicalTypeId::TIMESTAMP_NS:
		return DUCKDB_TYPE_TIMESTAMP_NS;
	case LogicalTypeId::DATE:
		return DUCKDB_TYPE_DATE;
	case LogicalTypeId::TIME:
		return DUCKDB_TYPE_TIME;
	case LogicalTypeId::VARCHAR:
		return DUCKDB_TYPE_VARCHAR;
	case LogicalTypeId::BLOB:
		return DUCKDB_TYPE_BLOB;
	case LogicalTypeId::INTERVAL:
		return DUCKDB_TYPE_INTERVAL;
	default:
		D_ASSERT(0);
		return DUCKDB_TYPE_INVALID;
	}
}

idx_t GetCTypeSize(duckdb_type type) {
	switch (type) {
	case DUCKDB_TYPE_BOOLEAN:
		return sizeof(bool);
	case DUCKDB_TYPE_TINYINT:
		return sizeof(int8_t);
	case DUCKDB_TYPE_SMALLINT:
		return sizeof(int16_t);
	case DUCKDB_TYPE_INTEGER:
		return sizeof(int32_t);
	case DUCKDB_TYPE_BIGINT:
		return sizeof(int64_t);
	case DUCKDB_TYPE_UTINYINT:
		return sizeof(uint8_t);
	case DUCKDB_TYPE_USMALLINT:
		return sizeof(uint16_t);
	case DUCKDB_TYPE_UINTEGER:
		return sizeof(uint32_t);
	case DUCKDB_TYPE_UBIGINT:
		return sizeof(uint64_t);
	case DUCKDB_TYPE_HUGEINT:
		return sizeof(duckdb_hugeint);
	case DUCKDB_TYPE_FLOAT:
		return sizeof(float);
	case DUCKDB_TYPE_DOUBLE:
		return sizeof(double);
	case DUCKDB_TYPE_DATE:
		return sizeof(duckdb_date);
	case DUCKDB_TYPE_TIME:
		return sizeof(duckdb_time);
	case DUCKDB_TYPE_TIMESTAMP:
	case DUCKDB_TYPE_TIMESTAMP_NS:
	case DUCKDB_TYPE_TIMESTAMP_MS:
	case DUCKDB_TYPE_TIMESTAMP_S:
		return sizeof(duckdb_timestamp);
	case DUCKDB_TYPE_VARCHAR:
		return sizeof(const char *);
	case DUCKDB_TYPE_BLOB:
		return sizeof(duckdb_blob);
	case DUCKDB_TYPE_INTERVAL:
		return sizeof(duckdb_interval);
	default:
		// unsupported type
		D_ASSERT(0);
		return sizeof(const char *);
	}
}

template <class T>
T UnsafeFetch(duckdb_result *result, idx_t col, idx_t row) {
	D_ASSERT(row < result->row_count);
	return ((T *)result->columns[col].data)[row];
}

static Value GetCValue(duckdb_result *result, idx_t col, idx_t row) {
	if (col >= result->column_count) {
		return Value();
	}
	if (row >= result->row_count) {
		return Value();
	}
	if (result->columns[col].nullmask[row]) {
		return Value();
	}
	switch (result->columns[col].type) {
	case DUCKDB_TYPE_BOOLEAN:
		return Value::BOOLEAN(UnsafeFetch<bool>(result, col, row));
	case DUCKDB_TYPE_TINYINT:
		return Value::TINYINT(UnsafeFetch<int8_t>(result, col, row));
	case DUCKDB_TYPE_SMALLINT:
		return Value::SMALLINT(UnsafeFetch<int16_t>(result, col, row));
	case DUCKDB_TYPE_INTEGER:
		return Value::INTEGER(UnsafeFetch<int32_t>(result, col, row));
	case DUCKDB_TYPE_BIGINT:
		return Value::BIGINT(UnsafeFetch<int64_t>(result, col, row));
	case DUCKDB_TYPE_UTINYINT:
		return Value::TINYINT(UnsafeFetch<uint8_t>(result, col, row));
	case DUCKDB_TYPE_USMALLINT:
		return Value::SMALLINT(UnsafeFetch<uint16_t>(result, col, row));
	case DUCKDB_TYPE_UINTEGER:
		return Value::INTEGER(UnsafeFetch<uint32_t>(result, col, row));
	case DUCKDB_TYPE_UBIGINT:
		return Value::BIGINT(UnsafeFetch<uint64_t>(result, col, row));
	case DUCKDB_TYPE_FLOAT:
		return Value(UnsafeFetch<float>(result, col, row));
	case DUCKDB_TYPE_DOUBLE:
		return Value(UnsafeFetch<double>(result, col, row));
	case DUCKDB_TYPE_DATE: {
		auto date = UnsafeFetch<duckdb_date>(result, col, row);
		return Value::DATE(date.year, date.month, date.day);
	}
	case DUCKDB_TYPE_TIME: {
		auto time = UnsafeFetch<duckdb_time>(result, col, row);
		return Value::TIME(time.hour, time.min, time.sec, time.micros);
	}
	case DUCKDB_TYPE_TIMESTAMP_NS:
	case DUCKDB_TYPE_TIMESTAMP_MS:
	case DUCKDB_TYPE_TIMESTAMP_S:
	case DUCKDB_TYPE_TIMESTAMP: {
		auto timestamp = UnsafeFetch<duckdb_timestamp>(result, col, row);
		return Value::TIMESTAMP(timestamp.date.year, timestamp.date.month, timestamp.date.day, timestamp.time.hour,
		                        timestamp.time.min, timestamp.time.sec, timestamp.time.micros);
	}
	case DUCKDB_TYPE_HUGEINT: {
		hugeint_t val;
		auto hugeint = UnsafeFetch<duckdb_hugeint>(result, col, row);
		val.lower = hugeint.lower;
		val.upper = hugeint.upper;
		return Value::HUGEINT(val);
	}
	case DUCKDB_TYPE_INTERVAL: {
		interval_t val;
		auto interval = UnsafeFetch<duckdb_interval>(result, col, row);
		val.days = interval.days;
		val.months = interval.months;
		val.micros = interval.micros;
		return Value::INTERVAL(val);
	}
	case DUCKDB_TYPE_VARCHAR:
		return Value(string(UnsafeFetch<const char *>(result, col, row)));
	case DUCKDB_TYPE_BLOB: {
		auto blob = UnsafeFetch<duckdb_blob>(result, col, row);
		return Value::BLOB((const_data_ptr_t)blob.data, blob.size);
	}
	default:
		// invalid type for C to C++ conversion
		D_ASSERT(0);
		return Value();
	}
}

const char *duckdb_column_name(duckdb_result *result, idx_t col) {
	if (!result || col >= result->column_count) {
		return nullptr;
	}
	return result->columns[col].name;
}

bool duckdb_value_boolean(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return false;
	} else {
		return val.GetValue<bool>();
	}
}

int8_t duckdb_value_int8(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<int8_t>();
	}
}

int16_t duckdb_value_int16(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<int16_t>();
	}
}

int32_t duckdb_value_int32(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<int32_t>();
	}
}

int64_t duckdb_value_int64(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<int64_t>();
	}
}

uint8_t duckdb_value_uint8(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<uint8_t>();
	}
}

uint16_t duckdb_value_uint16(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<uint16_t>();
	}
}

uint32_t duckdb_value_uint32(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<uint32_t>();
	}
}

uint64_t duckdb_value_uint64(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.GetValue<uint64_t>();
	}
}

float duckdb_value_float(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0.0;
	} else {
		return val.GetValue<float>();
	}
}

double duckdb_value_double(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0.0;
	} else {
		return val.GetValue<double>();
	}
}

char *duckdb_value_varchar(duckdb_result *result, idx_t col, idx_t row) {
	Value val = GetCValue(result, col, row);
	return strdup(val.ToString().c_str());
}

duckdb_blob duckdb_value_blob(duckdb_result *result, idx_t col, idx_t row) {
	duckdb_blob blob;
	Value val = GetCValue(result, col, row).CastAs(LogicalType::BLOB);
	if (val.is_null) {
		blob.data = nullptr;
		blob.size = 0;
	} else {
		blob.data = malloc(val.str_value.size());
		memcpy((void *)blob.data, val.str_value.c_str(), val.str_value.size());
		blob.size = val.str_value.size();
	}
	return blob;
}

void *duckdb_malloc(size_t size) {
	return malloc(size);
}

void duckdb_free(void *ptr) {
	free(ptr);
}

duckdb_state duckdb_appender_create(duckdb_connection connection, const char *schema, const char *table,
                                    duckdb_appender *out_appender) {
	Connection *conn = (Connection *)connection;

	if (!connection || !table || !out_appender) {
		return DuckDBError;
	}
	if (schema == nullptr) {

		schema = DEFAULT_SCHEMA;
	}
	try {
		auto *appender = new Appender(*conn, schema, table);
		*out_appender = appender;
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_appender_destroy(duckdb_appender *appender) {
	if (!appender || !*appender) {
		return DuckDBError;
	}
	auto *appender_instance = *((Appender **)appender);
	delete appender_instance;
	*appender = nullptr;
	return DuckDBSuccess;
}

template <class FUN>
duckdb_state duckdb_appender_run_function(duckdb_appender appender, FUN &&function) {
	if (!appender) {
		return DuckDBError;
	}
	auto *appender_instance = (Appender *)appender;
	try {
		function(*appender_instance);
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_appender_begin_row(duckdb_appender appender) {
	return duckdb_appender_run_function(appender, [&](Appender &appender) { appender.BeginRow(); });
}

duckdb_state duckdb_appender_end_row(duckdb_appender appender) {
	return duckdb_appender_run_function(appender, [&](Appender &appender) { appender.EndRow(); });
}

template <class T>
duckdb_state duckdb_append_internal(duckdb_appender appender, T value) {
	if (!appender) {
		return DuckDBError;
	}
	auto *appender_instance = (Appender *)appender;
	try {
		appender_instance->Append<T>(value);
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_append_bool(duckdb_appender appender, bool value) {
	return duckdb_append_internal<bool>(appender, value);
}

duckdb_state duckdb_append_int8(duckdb_appender appender, int8_t value) {
	return duckdb_append_internal<int8_t>(appender, value);
}

duckdb_state duckdb_append_int16(duckdb_appender appender, int16_t value) {
	return duckdb_append_internal<int16_t>(appender, value);
}

duckdb_state duckdb_append_int32(duckdb_appender appender, int32_t value) {
	return duckdb_append_internal<int32_t>(appender, value);
}

duckdb_state duckdb_append_int64(duckdb_appender appender, int64_t value) {
	return duckdb_append_internal<int64_t>(appender, value);
}

duckdb_state duckdb_append_uint8(duckdb_appender appender, uint8_t value) {
	return duckdb_append_internal<uint8_t>(appender, value);
}

duckdb_state duckdb_append_uint16(duckdb_appender appender, uint16_t value) {
	return duckdb_append_internal<uint16_t>(appender, value);
}

duckdb_state duckdb_append_uint32(duckdb_appender appender, uint32_t value) {
	return duckdb_append_internal<uint32_t>(appender, value);
}

duckdb_state duckdb_append_uint64(duckdb_appender appender, uint64_t value) {
	return duckdb_append_internal<uint64_t>(appender, value);
}

duckdb_state duckdb_append_float(duckdb_appender appender, float value) {
	return duckdb_append_internal<float>(appender, value);
}

duckdb_state duckdb_append_double(duckdb_appender appender, double value) {
	return duckdb_append_internal<double>(appender, value);
}

duckdb_state duckdb_append_null(duckdb_appender appender) {
	return duckdb_append_internal<std::nullptr_t>(appender, nullptr);
}

duckdb_state duckdb_append_varchar(duckdb_appender appender, const char *val) {
	return duckdb_append_internal<Value>(appender, Value(val));
}

duckdb_state duckdb_append_varchar_length(duckdb_appender appender, const char *val, idx_t length) {
	return duckdb_append_internal<Value>(appender, Value(string(val, length)));
}
duckdb_state duckdb_append_blob(duckdb_appender appender, const void *data, idx_t length) {
	return duckdb_append_internal<Value>(appender, Value::BLOB((const_data_ptr_t)data, length));
}

duckdb_state duckdb_appender_flush(duckdb_appender appender) {
	return duckdb_appender_run_function(appender, [&](Appender &appender) { appender.Flush(); });
}

duckdb_state duckdb_appender_close(duckdb_appender appender) {
	return duckdb_appender_run_function(appender, [&](Appender &appender) { appender.Close(); });
}
