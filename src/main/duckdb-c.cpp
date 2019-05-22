#include "common/types/date.hpp"
#include "common/vector_operations/vector_operations.hpp"
#include "duckdb.h"
#include "duckdb.hpp"

#include <cstring>

using namespace duckdb;

static SQLType ConvertCTypeToCPP(duckdb_type type);
static duckdb_type ConvertCPPTypeToC(SQLType type);
static count_t GetCTypeSize(duckdb_type type);

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

template <class T> void WriteData(duckdb_result *out, ChunkCollection &source, index_t col) {
	index_t row = 0;
	T *target = (T *)out->columns[col].data;
	for (auto &chunk : source.chunks) {
		T *source = (T *)chunk->data[col].data;
		for (index_t k = 0; k < chunk->data[col].count; k++) {
			target[row++] = source[k];
		}
	}
}

duckdb_state duckdb_query(duckdb_connection connection, const char *query, duckdb_result *out) {
	Connection *conn = (Connection *)connection;
	auto result = conn->Query(query);
	if (!out) {
		// no result to write to, only return the status
		return result->success ? DuckDBSuccess : DuckDBError;
	}
	out->error_message = NULL;
	if (!result->success) {
		// write the error message
		out->error_message = strdup(result->error.c_str());
		return DuckDBError;
	}
	// copy the data
	// first write the meta data
	out->column_count = result->types.size();
	out->row_count = result->collection.count;
	out->columns = (duckdb_column *)malloc(sizeof(duckdb_column) * out->column_count);
	if (!out->columns) {
		return DuckDBError;
	}
	// zero initialize the columns (so we can cleanly delete it in case a malloc fails)
	memset(out->columns, 0, sizeof(duckdb_column) * out->column_count);
	for (index_t i = 0; i < out->column_count; i++) {
		out->columns[i].type = ConvertCPPTypeToC(result->sql_types[i]);
		out->columns[i].name = strdup(result->names[i].c_str());
		out->columns[i].nullmask = (bool *)malloc(sizeof(bool) * out->row_count);
		out->columns[i].data = malloc(GetCTypeSize(out->columns[i].type) * out->row_count);
		if (!out->columns[i].nullmask || !out->columns[i].name || !out->columns[i].data) {
			// malloc failure
			return DuckDBError;
		}
		// memset data to 0 for VARCHAR columns for safe deletion later
		if (result->types[i] == TypeId::VARCHAR) {
			memset(out->columns[i].data, 0, GetCTypeSize(out->columns[i].type) * out->row_count);
		}
	}
	// now write the data
	for (index_t col = 0; col < out->column_count; col++) {
		// first set the nullmask
		index_t row = 0;
		for (auto &chunk : result->collection.chunks) {
			assert(!chunk->data[col].sel_vector);
			for (index_t k = 0; k < chunk->data[col].count; k++) {
				out->columns[col].nullmask[row++] = chunk->data[col].nullmask[k];
			}
		}
		// then write the data
		switch (result->sql_types[col].id) {
		case SQLTypeId::BOOLEAN:
			WriteData<bool>(out, result->collection, col);
			break;
		case SQLTypeId::TINYINT:
			WriteData<int8_t>(out, result->collection, col);
			break;
		case SQLTypeId::SMALLINT:
			WriteData<int16_t>(out, result->collection, col);
			break;
		case SQLTypeId::INTEGER:
			WriteData<int32_t>(out, result->collection, col);
			break;
		case SQLTypeId::BIGINT:
			WriteData<int64_t>(out, result->collection, col);
			break;
		case SQLTypeId::FLOAT:
			WriteData<float>(out, result->collection, col);
			break;
		case SQLTypeId::DECIMAL:
		case SQLTypeId::DOUBLE:
			WriteData<double>(out, result->collection, col);
			break;
		case SQLTypeId::VARCHAR: {
			index_t row = 0;
			const char **target = (const char **)out->columns[col].data;
			for (auto &chunk : result->collection.chunks) {
				const char **source = (const char **)chunk->data[col].data;
				for (index_t k = 0; k < chunk->data[col].count; k++) {
					if (!chunk->data[col].nullmask[k]) {
						target[row] = strdup(source[k]);
					}
					row++;
				}
			}
			break;
		}
		case SQLTypeId::DATE: {
			index_t row = 0;
			duckdb_date *target = (duckdb_date *)out->columns[col].data;
			for (auto &chunk : result->collection.chunks) {
				date_t *source = (date_t *)chunk->data[col].data;
				for (index_t k = 0; k < chunk->data[col].count; k++) {
					if (!chunk->data[col].nullmask[k]) {
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
		case SQLTypeId::TIMESTAMP:
		default:
			// unsupported type for C API
			assert(0);
			return DuckDBError;
		}
	}
	return DuckDBSuccess;
}

static void duckdb_destroy_column(duckdb_column column, count_t count) {
	if (column.data) {
		if (column.type == DUCKDB_TYPE_VARCHAR) {
			// varchar, delete individual strings
			auto data = (char **)column.data;
			for (index_t i = 0; i < count; i++) {
				if (data[i]) {
					free(data[i]);
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
		for (index_t i = 0; i < result->column_count; i++) {
			duckdb_destroy_column(result->columns[i], result->row_count);
		}
		free(result->columns);
	}
	memset(result, 0, sizeof(duckdb_result));
}

duckdb_type ConvertCPPTypeToC(SQLType sql_type) {
	switch (sql_type.id) {
	case SQLTypeId::BOOLEAN:
		return DUCKDB_TYPE_BOOLEAN;
	case SQLTypeId::TINYINT:
		return DUCKDB_TYPE_TINYINT;
	case SQLTypeId::SMALLINT:
		return DUCKDB_TYPE_SMALLINT;
	case SQLTypeId::INTEGER:
		return DUCKDB_TYPE_INTEGER;
	case SQLTypeId::BIGINT:
		return DUCKDB_TYPE_BIGINT;
	case SQLTypeId::FLOAT:
		return DUCKDB_TYPE_FLOAT;
	case SQLTypeId::DECIMAL:
	case SQLTypeId::DOUBLE:
		return DUCKDB_TYPE_DOUBLE;
	case SQLTypeId::TIMESTAMP:
		return DUCKDB_TYPE_TIMESTAMP;
	case SQLTypeId::DATE:
		return DUCKDB_TYPE_DATE;
	case SQLTypeId::VARCHAR:
		return DUCKDB_TYPE_VARCHAR;
	default:
		return DUCKDB_TYPE_INVALID;
	}
}

SQLType ConvertCTypeToCPP(duckdb_type type) {
	switch (type) {
	case DUCKDB_TYPE_BOOLEAN:
		return SQLType(SQLTypeId::BOOLEAN);
	case DUCKDB_TYPE_TINYINT:
		return SQLType(SQLTypeId::TINYINT);
	case DUCKDB_TYPE_SMALLINT:
		return SQLType(SQLTypeId::SMALLINT);
	case DUCKDB_TYPE_INTEGER:
		return SQLType(SQLTypeId::INTEGER);
	case DUCKDB_TYPE_BIGINT:
		return SQLType(SQLTypeId::BIGINT);
	case DUCKDB_TYPE_FLOAT:
		return SQLType(SQLTypeId::FLOAT);
	case DUCKDB_TYPE_DOUBLE:
		return SQLType(SQLTypeId::DOUBLE);
	case DUCKDB_TYPE_TIMESTAMP:
		return SQLType(SQLTypeId::TIMESTAMP);
	case DUCKDB_TYPE_DATE:
		return SQLType(SQLTypeId::DATE);
	case DUCKDB_TYPE_VARCHAR:
		return SQLType(SQLTypeId::VARCHAR);
	default:
		return SQLType(SQLTypeId::INVALID);
	}
}

count_t GetCTypeSize(duckdb_type type) {
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
	case DUCKDB_TYPE_FLOAT:
		return sizeof(float);
	case DUCKDB_TYPE_DOUBLE:
		return sizeof(double);
	case DUCKDB_TYPE_DATE:
		return sizeof(duckdb_date);
	case DUCKDB_TYPE_VARCHAR:
		return sizeof(const char *);
	default:
		// unsupported type
		assert(0);
		return sizeof(const char *);
	}
}

template <class T> T UnsafeFetch(duckdb_result *result, index_t col, index_t row) {
	assert(row < result->row_count);
	return ((T *)result->columns[col].data)[row];
}

static Value GetCValue(duckdb_result *result, index_t col, index_t row) {
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
	case DUCKDB_TYPE_FLOAT:
		return Value(UnsafeFetch<float>(result, col, row));
	case DUCKDB_TYPE_DOUBLE:
		return Value(UnsafeFetch<double>(result, col, row));
	case DUCKDB_TYPE_DATE: {
		auto date = UnsafeFetch<duckdb_date>(result, col, row);
		return Value::INTEGER(Date::FromDate(date.year, date.month, date.day));
	}
	case DUCKDB_TYPE_VARCHAR:
		return Value(string(UnsafeFetch<const char *>(result, col, row)));
	// case DUCKDB_TYPE_TIMESTAMP:
	default:
		// invalid type for C to C++ conversion
		assert(0);
		return Value();
	}
}

bool duckdb_value_boolean(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return false;
	} else {
		return val.CastAs(TypeId::BOOLEAN).value_.boolean;
	}
}

int8_t duckdb_value_int8(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.CastAs(TypeId::TINYINT).value_.tinyint;
	}
}

int16_t duckdb_value_int16(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.CastAs(TypeId::SMALLINT).value_.smallint;
	}
}

int32_t duckdb_value_int32(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.CastAs(TypeId::INTEGER).value_.integer;
	}
}

int64_t duckdb_value_int64(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0;
	} else {
		return val.CastAs(TypeId::BIGINT).value_.bigint;
	}
}

float duckdb_value_float(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0.0;
	} else {
		return val.CastAs(TypeId::FLOAT).value_.float_;
	}
}

double duckdb_value_double(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	if (val.is_null) {
		return 0.0;
	} else {
		return val.CastAs(TypeId::DOUBLE).value_.double_;
	}
}

char *duckdb_value_varchar(duckdb_result *result, index_t col, index_t row) {
	Value val = GetCValue(result, col, row);
	return strdup(val.ToString(ConvertCTypeToCPP(result->columns[col].type)).c_str());
}
