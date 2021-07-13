//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// duckdb.h
//
// Author: Mark Raasveldt
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef _WIN32
#ifdef DUCKDB_BUILD_LIBRARY
#define DUCKDB_API __declspec(dllexport)
#else
#define DUCKDB_API __declspec(dllimport)
#endif
#else
#define DUCKDB_API
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t idx_t;

typedef enum DUCKDB_TYPE {
	DUCKDB_TYPE_INVALID = 0,
	// bool
	DUCKDB_TYPE_BOOLEAN,
	// int8_t
	DUCKDB_TYPE_TINYINT,
	// int16_t
	DUCKDB_TYPE_SMALLINT,
	// int32_t
	DUCKDB_TYPE_INTEGER,
	// int64_t
	DUCKDB_TYPE_BIGINT,
	// uint8_t
	DUCKDB_TYPE_UTINYINT,
	// uint16_t
	DUCKDB_TYPE_USMALLINT,
	// uint32_t
	DUCKDB_TYPE_UINTEGER,
	// uint64_t
	DUCKDB_TYPE_UBIGINT,
	// float
	DUCKDB_TYPE_FLOAT,
	// double
	DUCKDB_TYPE_DOUBLE,
	// duckdb_timestamp (us)
	DUCKDB_TYPE_TIMESTAMP,
	// duckdb_timestamp (s)
	DUCKDB_TYPE_TIMESTAMP_S,
	// duckdb_timestamp (ns)
	DUCKDB_TYPE_TIMESTAMP_NS,
	// duckdb_timestamp (ms)
	DUCKDB_TYPE_TIMESTAMP_MS,
	// duckdb_date
	DUCKDB_TYPE_DATE,
	// duckdb_time
	DUCKDB_TYPE_TIME,
	// duckdb_interval
	DUCKDB_TYPE_INTERVAL,
	// duckdb_hugeint
	DUCKDB_TYPE_HUGEINT,
	// const char*
	DUCKDB_TYPE_VARCHAR,
	// duckdb_blob
	DUCKDB_TYPE_BLOB
} duckdb_type;

typedef struct {
	int32_t year;
	int8_t month;
	int8_t day;
} duckdb_date;

typedef struct {
	int8_t hour;
	int8_t min;
	int8_t sec;
	int32_t micros;
} duckdb_time;

typedef struct {
	duckdb_date date;
	duckdb_time time;
} duckdb_timestamp;

typedef struct {
	int32_t months;
	int32_t days;
	int64_t micros;
} duckdb_interval;

typedef struct {
	uint64_t lower;
	int64_t upper;
} duckdb_hugeint;

typedef struct {
	void *data;
	idx_t size;
} duckdb_blob;

typedef struct {
	void *data;
	bool *nullmask;
	duckdb_type type;
	char *name;
} duckdb_column;

typedef struct {
	idx_t column_count;
	idx_t row_count;
	idx_t rows_changed;
	duckdb_column *columns;
	char *error_message;
} duckdb_result;

// typedef struct {
// 	void *data;
// 	bool *nullmask;
// } duckdb_column_data;

// typedef struct {
// 	int column_count;
// 	int count;
// 	duckdb_column_data *columns;
// } duckdb_chunk;

typedef void *duckdb_database;
typedef void *duckdb_connection;
typedef void *duckdb_prepared_statement;
typedef void *duckdb_appender;
typedef void *duckdb_arrow;
// we don't need to spell out the schema/array in here
// because it's a common interface, users can consume
// the data in their own logic.
typedef void *duckdb_arrow_schema;
typedef void *duckdb_arrow_array;

typedef enum { DuckDBSuccess = 0, DuckDBError = 1 } duckdb_state;

//! query duckdb result as arrow data structure
DUCKDB_API duckdb_state duckdb_query_arrow(duckdb_connection connection, const char *query, duckdb_arrow *out_result);
//! get arrow schema
DUCKDB_API duckdb_state duckdb_query_arrow_schema(duckdb_arrow result, duckdb_arrow_schema *out_schema);
//! get arrow data array
//! This function can be called multiple time to get next chunks, which will free the previous out_array.
//! So consume the out_array before call this function again
DUCKDB_API duckdb_state duckdb_query_arrow_array(duckdb_arrow result, duckdb_arrow_array *out_array);
//! get arrow error message
DUCKDB_API const char *duckdb_query_arrow_error(duckdb_arrow result);
//! Destroys the arrow result
DUCKDB_API void duckdb_destroy_arrow(duckdb_arrow *result);

//! Opens a database file at the given path (nullptr for in-memory). Returns DuckDBSuccess on success, or DuckDBError on
//! failure. [OUT: database]
DUCKDB_API duckdb_state duckdb_open(const char *path, duckdb_database *out_database);
//! Closes the database.
DUCKDB_API void duckdb_close(duckdb_database *database);

//! Creates a connection to the specified database. [OUT: connection]
DUCKDB_API duckdb_state duckdb_connect(duckdb_database database, duckdb_connection *out_connection);
//! Closes the specified connection handle
DUCKDB_API void duckdb_disconnect(duckdb_connection *connection);

//! Executes the specified SQL query in the specified connection handle. [OUT: result descriptor]
DUCKDB_API duckdb_state duckdb_query(duckdb_connection connection, const char *query, duckdb_result *out_result);
//! Destroys the specified result
DUCKDB_API void duckdb_destroy_result(duckdb_result *result);

//! Returns the column name of the specified column. The result does not need to be freed;
//! the column names will automatically be destroyed when the result is destroyed.
DUCKDB_API const char *duckdb_column_name(duckdb_result *result, idx_t col);

// SAFE fetch functions
// These functions will perform conversions if necessary. On failure (e.g. if conversion cannot be performed) a special
// value is returned.

//! Converts the specified value to a bool. Returns false on failure or NULL.
DUCKDB_API bool duckdb_value_boolean(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an int8_t. Returns 0 on failure or NULL.
DUCKDB_API int8_t duckdb_value_int8(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an int16_t. Returns 0 on failure or NULL.
DUCKDB_API int16_t duckdb_value_int16(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an int64_t. Returns 0 on failure or NULL.
DUCKDB_API int32_t duckdb_value_int32(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an int64_t. Returns 0 on failure or NULL.
DUCKDB_API int64_t duckdb_value_int64(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an uint8_t. Returns 0 on failure or NULL.
DUCKDB_API uint8_t duckdb_value_uint8(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an uint16_t. Returns 0 on failure or NULL.
DUCKDB_API uint16_t duckdb_value_uint16(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an uint64_t. Returns 0 on failure or NULL.
DUCKDB_API uint32_t duckdb_value_uint32(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to an uint64_t. Returns 0 on failure or NULL.
DUCKDB_API uint64_t duckdb_value_uint64(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to a float. Returns 0.0 on failure or NULL.
DUCKDB_API float duckdb_value_float(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to a double. Returns 0.0 on failure or NULL.
DUCKDB_API double duckdb_value_double(duckdb_result *result, idx_t col, idx_t row);
//! Converts the specified value to a string. Returns nullptr on failure or NULL. The result must be freed with
//! duckdb_free.
DUCKDB_API char *duckdb_value_varchar(duckdb_result *result, idx_t col, idx_t row);
//! Fetches a blob from a result set column. Returns a blob with blob.data set to nullptr on failure or NULL. The
//! resulting "blob.data" must be freed with duckdb_free.
DUCKDB_API duckdb_blob duckdb_value_blob(duckdb_result *result, idx_t col, idx_t row);

//! Allocate [size] amounts of memory using the duckdb internal malloc function. Any memory allocated in this manner
//! should be freed using duckdb_free
DUCKDB_API void *duckdb_malloc(size_t size);
//! Free a value returned from duckdb_malloc, duckdb_value_varchar or duckdb_value_blob
DUCKDB_API void duckdb_free(void *ptr);

// Prepared Statements

//! prepares the specified SQL query in the specified connection handle. [OUT: prepared statement descriptor]
DUCKDB_API duckdb_state duckdb_prepare(duckdb_connection connection, const char *query,
                                       duckdb_prepared_statement *out_prepared_statement);

DUCKDB_API const char *duckdb_prepare_error(duckdb_prepared_statement prepared_statement);
DUCKDB_API duckdb_state duckdb_nparams(duckdb_prepared_statement prepared_statement, idx_t *nparams_out);

//! binds parameters to prepared statement
DUCKDB_API duckdb_state duckdb_bind_boolean(duckdb_prepared_statement prepared_statement, idx_t param_idx, bool val);
DUCKDB_API duckdb_state duckdb_bind_int8(duckdb_prepared_statement prepared_statement, idx_t param_idx, int8_t val);
DUCKDB_API duckdb_state duckdb_bind_int16(duckdb_prepared_statement prepared_statement, idx_t param_idx, int16_t val);
DUCKDB_API duckdb_state duckdb_bind_int32(duckdb_prepared_statement prepared_statement, idx_t param_idx, int32_t val);
DUCKDB_API duckdb_state duckdb_bind_int64(duckdb_prepared_statement prepared_statement, idx_t param_idx, int64_t val);
DUCKDB_API duckdb_state duckdb_bind_uint8(duckdb_prepared_statement prepared_statement, idx_t param_idx, int8_t val);
DUCKDB_API duckdb_state duckdb_bind_uint16(duckdb_prepared_statement prepared_statement, idx_t param_idx, int16_t val);
DUCKDB_API duckdb_state duckdb_bind_uint32(duckdb_prepared_statement prepared_statement, idx_t param_idx, uint32_t val);
DUCKDB_API duckdb_state duckdb_bind_uint64(duckdb_prepared_statement prepared_statement, idx_t param_idx, uint64_t val);
DUCKDB_API duckdb_state duckdb_bind_float(duckdb_prepared_statement prepared_statement, idx_t param_idx, float val);
DUCKDB_API duckdb_state duckdb_bind_double(duckdb_prepared_statement prepared_statement, idx_t param_idx, double val);
DUCKDB_API duckdb_state duckdb_bind_varchar(duckdb_prepared_statement prepared_statement, idx_t param_idx,
                                            const char *val);
DUCKDB_API duckdb_state duckdb_bind_varchar_length(duckdb_prepared_statement prepared_statement, idx_t param_idx,
                                                   const char *val, idx_t length);
DUCKDB_API duckdb_state duckdb_bind_blob(duckdb_prepared_statement prepared_statement, idx_t param_idx,
                                         const void *data, idx_t length);
DUCKDB_API duckdb_state duckdb_bind_null(duckdb_prepared_statement prepared_statement, idx_t param_idx);

//! Executes the prepared statements with currently bound parameters
DUCKDB_API duckdb_state duckdb_execute_prepared(duckdb_prepared_statement prepared_statement,
                                                duckdb_result *out_result);

//! Executes the prepared statements with currently bound parameters and return arrow result
DUCKDB_API duckdb_state duckdb_execute_prepared_arrow(duckdb_prepared_statement prepared_statement,
                                                      duckdb_arrow *out_result);

//! Destroys the specified prepared statement descriptor
DUCKDB_API void duckdb_destroy_prepare(duckdb_prepared_statement *prepared_statement);

DUCKDB_API duckdb_state duckdb_appender_create(duckdb_connection connection, const char *schema, const char *table,
                                               duckdb_appender *out_appender);

DUCKDB_API duckdb_state duckdb_appender_begin_row(duckdb_appender appender);
DUCKDB_API duckdb_state duckdb_appender_end_row(duckdb_appender appender);

DUCKDB_API duckdb_state duckdb_append_bool(duckdb_appender appender, bool value);

DUCKDB_API duckdb_state duckdb_append_int8(duckdb_appender appender, int8_t value);
DUCKDB_API duckdb_state duckdb_append_int16(duckdb_appender appender, int16_t value);
DUCKDB_API duckdb_state duckdb_append_int32(duckdb_appender appender, int32_t value);
DUCKDB_API duckdb_state duckdb_append_int64(duckdb_appender appender, int64_t value);

DUCKDB_API duckdb_state duckdb_append_uint8(duckdb_appender appender, uint8_t value);
DUCKDB_API duckdb_state duckdb_append_uint16(duckdb_appender appender, uint16_t value);
DUCKDB_API duckdb_state duckdb_append_uint32(duckdb_appender appender, uint32_t value);
DUCKDB_API duckdb_state duckdb_append_uint64(duckdb_appender appender, uint64_t value);

DUCKDB_API duckdb_state duckdb_append_float(duckdb_appender appender, float value);
DUCKDB_API duckdb_state duckdb_append_double(duckdb_appender appender, double value);

DUCKDB_API duckdb_state duckdb_append_varchar(duckdb_appender appender, const char *val);
DUCKDB_API duckdb_state duckdb_append_varchar_length(duckdb_appender appender, const char *val, idx_t length);
DUCKDB_API duckdb_state duckdb_append_blob(duckdb_appender appender, const void *data, idx_t length);
DUCKDB_API duckdb_state duckdb_append_null(duckdb_appender appender);

DUCKDB_API duckdb_state duckdb_appender_flush(duckdb_appender appender);
DUCKDB_API duckdb_state duckdb_appender_close(duckdb_appender appender);

DUCKDB_API duckdb_state duckdb_appender_destroy(duckdb_appender *appender);

#ifdef __cplusplus
}
#endif
