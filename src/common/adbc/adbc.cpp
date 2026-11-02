#include "duckdb/common/adbc/adbc.hpp"
#include "duckdb/common/adbc/adbc-init.hpp"

#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"

#include "duckdb.h"
#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/arrow/nanoarrow/nanoarrow.hpp"
#include "duckdb/main/query_parameters.hpp"
#include "duckdb/main/query_result_stream.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/common/adbc/options.h"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/common/adbc/wrappers.hpp"
#include <algorithm>
#include <cstring>
#include <stdlib.h>
static void ReleaseError(struct AdbcError *error);
static void ReleaseErrorWithDuckDBDetails(struct AdbcError *error);
static void ReleaseStreamErrorDetails(struct AdbcError *error);
static const char *DuckDBErrorTypeToString(duckdb_error_type type);
static void AppendDuckDBErrorDetails(struct AdbcError *error, duckdb_error_type type);
static void AppendDuckDBErrorDetails(struct AdbcError *error, duckdb_error_data res);

struct DuckDBErrorDetails {
	std::vector<std::pair<std::string, std::string>> entries;
};

#include <string.h>

#include "duckdb/parser/keyword_helper.hpp"

// We must leak the symbols of the init function
AdbcStatusCode duckdb_adbc_init(int version, void *driver, struct AdbcError *error) {
	if (!driver) {
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	// Check that the version is supported (1.0.0 or 1.1.0)
	if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0) {
		return ADBC_STATUS_NOT_IMPLEMENTED;
	}

	auto adbc_driver = static_cast<AdbcDriver *>(driver);

	// Initialize all 1.0.0 function pointers
	adbc_driver->DatabaseNew = duckdb_adbc::DatabaseNew;
	adbc_driver->DatabaseSetOption = duckdb_adbc::DatabaseSetOption;
	adbc_driver->DatabaseInit = duckdb_adbc::DatabaseInit;
	adbc_driver->DatabaseRelease = duckdb_adbc::DatabaseRelease;
	adbc_driver->ConnectionNew = duckdb_adbc::ConnectionNew;
	adbc_driver->ConnectionSetOption = duckdb_adbc::ConnectionSetOption;
	adbc_driver->ConnectionInit = duckdb_adbc::ConnectionInit;
	adbc_driver->ConnectionRelease = duckdb_adbc::ConnectionRelease;
	adbc_driver->ConnectionGetTableTypes = duckdb_adbc::ConnectionGetTableTypes;
	adbc_driver->StatementNew = duckdb_adbc::StatementNew;
	adbc_driver->StatementRelease = duckdb_adbc::StatementRelease;
	adbc_driver->StatementBind = duckdb_adbc::StatementBind;
	adbc_driver->StatementBindStream = duckdb_adbc::StatementBindStream;
	adbc_driver->StatementExecuteQuery = duckdb_adbc::StatementExecuteQuery;
	adbc_driver->StatementPrepare = duckdb_adbc::StatementPrepare;
	adbc_driver->StatementSetOption = duckdb_adbc::StatementSetOption;
	adbc_driver->StatementSetSqlQuery = duckdb_adbc::StatementSetSqlQuery;
	adbc_driver->ConnectionGetObjects = duckdb_adbc::ConnectionGetObjects;
	adbc_driver->ConnectionCommit = duckdb_adbc::ConnectionCommit;
	adbc_driver->ConnectionRollback = duckdb_adbc::ConnectionRollback;
	adbc_driver->ConnectionReadPartition = duckdb_adbc::ConnectionReadPartition;
	adbc_driver->StatementExecutePartitions = duckdb_adbc::StatementExecutePartitions;
	adbc_driver->ConnectionGetInfo = duckdb_adbc::ConnectionGetInfo;
	adbc_driver->StatementGetParameterSchema = duckdb_adbc::StatementGetParameterSchema;
	adbc_driver->ConnectionGetTableSchema = duckdb_adbc::ConnectionGetTableSchema;

	// Initialize 1.1.0 function pointers if version >= 1.1.0
	if (version >= ADBC_VERSION_1_1_0) {
		adbc_driver->ErrorGetDetailCount = duckdb_adbc::ErrorGetDetailCount;
		adbc_driver->ErrorGetDetail = duckdb_adbc::ErrorGetDetail;
		adbc_driver->ErrorFromArrayStream = duckdb_adbc::ErrorFromArrayStream;

		adbc_driver->DatabaseGetOption = duckdb_adbc::DatabaseGetOption;
		adbc_driver->DatabaseGetOptionBytes = duckdb_adbc::DatabaseGetOptionBytes;
		adbc_driver->DatabaseGetOptionDouble = duckdb_adbc::DatabaseGetOptionDouble;
		adbc_driver->DatabaseGetOptionInt = duckdb_adbc::DatabaseGetOptionInt;
		adbc_driver->DatabaseSetOptionBytes = duckdb_adbc::DatabaseSetOptionBytes;
		adbc_driver->DatabaseSetOptionInt = duckdb_adbc::DatabaseSetOptionInt;
		adbc_driver->DatabaseSetOptionDouble = duckdb_adbc::DatabaseSetOptionDouble;

		adbc_driver->ConnectionCancel = duckdb_adbc::ConnectionCancel;
		adbc_driver->ConnectionGetOption = duckdb_adbc::ConnectionGetOption;
		adbc_driver->ConnectionGetOptionBytes = duckdb_adbc::ConnectionGetOptionBytes;
		adbc_driver->ConnectionGetOptionDouble = duckdb_adbc::ConnectionGetOptionDouble;
		adbc_driver->ConnectionGetOptionInt = duckdb_adbc::ConnectionGetOptionInt;
		adbc_driver->ConnectionGetStatistics = duckdb_adbc::ConnectionGetStatistics;
		adbc_driver->ConnectionGetStatisticNames = duckdb_adbc::ConnectionGetStatisticNames;
		adbc_driver->ConnectionSetOptionBytes = duckdb_adbc::ConnectionSetOptionBytes;
		adbc_driver->ConnectionSetOptionInt = duckdb_adbc::ConnectionSetOptionInt;
		adbc_driver->ConnectionSetOptionDouble = duckdb_adbc::ConnectionSetOptionDouble;

		adbc_driver->StatementCancel = duckdb_adbc::StatementCancel;
		adbc_driver->StatementExecuteSchema = duckdb_adbc::StatementExecuteSchema;
		adbc_driver->StatementGetOption = duckdb_adbc::StatementGetOption;
		adbc_driver->StatementGetOptionBytes = duckdb_adbc::StatementGetOptionBytes;
		adbc_driver->StatementGetOptionDouble = duckdb_adbc::StatementGetOptionDouble;
		adbc_driver->StatementGetOptionInt = duckdb_adbc::StatementGetOptionInt;
		adbc_driver->StatementSetOptionBytes = duckdb_adbc::StatementSetOptionBytes;
		adbc_driver->StatementSetOptionDouble = duckdb_adbc::StatementSetOptionDouble;
		adbc_driver->StatementSetOptionInt = duckdb_adbc::StatementSetOptionInt;
	}

	return ADBC_STATUS_OK;
}

namespace duckdb_adbc {

// ADBC 1.1.0: Added REPLACE and CREATE_APPEND modes
enum class IngestionMode { CREATE = 0, APPEND = 1, REPLACE = 2, CREATE_APPEND = 3 };

struct DuckDBAdbcStatementWrapper {
	duckdb_connection connection;
	duckdb::DuckDBAdbcConnectionWrapper *conn_wrapper;
	duckdb_prepared_statement statement;
	char *ingestion_table_name;
	char *target_catalog;
	char *db_schema;
	ArrowArrayStream ingestion_stream;
	IngestionMode ingestion_mode = IngestionMode::CREATE;
	bool temporary_table = false;
	//! Metadata calls complete their result at submit, so they snapshot the catalog at the call
	bool eager_result = false;
	uint64_t plan_length;
	//! The number of this statement's last execution on its connection, 0 until it first executes. Read by the
	//! progress options from any thread.
	duckdb::atomic<uint64_t> execution {0};
	//! Whether that execution ran to completion. Only meaningful once it is no longer the running one.
	duckdb::atomic<bool> completed {false};
	//! The highest progress seen, packed with the execution it was measured for, so that a reading taken just before
	//! a new execution started cannot raise the floor of that new execution. See PackProgress().
	duckdb::atomic<uint64_t> progress_floor {0};
	//! Set when the connection is released while this statement still exists: `connection` and `conn_wrapper` dangle
	//! from then on, and the progress options must not follow them.
	duckdb::atomic<bool> connection_released {false};
};

struct DuckDBAdbcStreamWrapper {
	~DuckDBAdbcStreamWrapper() {
		free(last_error);
		InitializeADBCError(&adbc_error);
	}

	//! Drained into `materialized` and reset when another statement runs on the connection
	duckdb::unique_ptr<duckdb::QueryResultStream<duckdb::ArrowFormat>> stream;
	//! A result the driver completed before handing it out, which no later statement on the connection affects
	duckdb::unique_ptr<duckdb::QueryResult> result;
	duckdb::ArrowSchemaWrapper schema;
	//! What draining collected, handed out before the error it ran into, if any
	duckdb::vector<duckdb::unique_ptr<duckdb::ArrowArrayWrapper>> materialized;
	//! Serializes a reader with a drain or a close; per stream, so neither blocks another stream's reader
	duckdb::mutex fetch_mutex;
	idx_t materialized_index = 0;
	char *last_error = nullptr;
	AdbcStatusCode status_code = ADBC_STATUS_OK;
	AdbcError adbc_error = {};
	duckdb::DuckDBAdbcConnectionWrapper *conn_wrapper = nullptr;
	//! Reached through the connection registry, since a stream can outlive its statement.
	uint64_t execution = 0;
	DuckDBAdbcStatementWrapper *statement = nullptr;
};

//! Takes the connection's query progress for one execution. The execution ends when the guard goes out of scope,
//! unless its result is handed out as a stream, which then ends it when the stream is exhausted, fails, is released
//! or is materialized. An execution that is not marked completed is reported as having ended without completing.
class ExecutionProgressGuard {
public:
	ExecutionProgressGuard(duckdb::DuckDBAdbcConnectionWrapper *conn_wrapper_p, DuckDBAdbcStatementWrapper *statement_p)
	    : conn_wrapper(conn_wrapper_p), statement(statement_p) {
		if (conn_wrapper) {
			execution = conn_wrapper->BeginExecution();
		}
	}
	ExecutionProgressGuard(const ExecutionProgressGuard &) = delete;
	ExecutionProgressGuard &operator=(const ExecutionProgressGuard &) = delete;

	~ExecutionProgressGuard() {
		if (conn_wrapper) {
			conn_wrapper->FinishExecution(statement, execution, completed);
		}
	}

	uint64_t Execution() const {
		return execution;
	}

	//! The work of this execution is done; without this it is reported as having ended without completing.
	void Completed() {
		completed = true;
	}

	void HandOver() {
		conn_wrapper = nullptr;
	}

private:
	duckdb::DuckDBAdbcConnectionWrapper *conn_wrapper;
	DuckDBAdbcStatementWrapper *statement;
	uint64_t execution = 0;
	bool completed = false;
};

static bool IsCancellation(duckdb::optional_ptr<duckdb::DuckDBAdbcConnectionWrapper> conn_wrapper,
                           duckdb::ExceptionType type) {
	return type == duckdb::ExceptionType::INTERRUPT && conn_wrapper && conn_wrapper->cancel_requested;
}

static void SetStreamError(DuckDBAdbcStreamWrapper &wrapper, const duckdb::ErrorData &error) {
	free(wrapper.last_error);
	wrapper.last_error = strdup(error.Message().c_str());
	wrapper.status_code =
	    IsCancellation(wrapper.conn_wrapper, error.Type()) ? ADBC_STATUS_CANCELLED : ADBC_STATUS_INTERNAL;
	wrapper.adbc_error.message = wrapper.last_error;
	wrapper.adbc_error.vendor_code = ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA;
	delete static_cast<DuckDBErrorDetails *>(wrapper.adbc_error.private_data);
	wrapper.adbc_error.private_data = nullptr;
	wrapper.adbc_error.release = nullptr;
	auto *details = new (std::nothrow) DuckDBErrorDetails();
	if (details) {
		details->entries.emplace_back("duckdb:error_type", DuckDBErrorTypeToString(duckdb::ErrorTypeToC(error.Type())));
		wrapper.adbc_error.private_data = details;
		wrapper.adbc_error.release = ::ReleaseStreamErrorDetails;
	}
}

//! The next array, null at the end of the result and on an error, which goes to `error`
static duckdb::unique_ptr<duckdb::ArrowArrayWrapper> FetchArray(DuckDBAdbcStreamWrapper &wrapper,
                                                                duckdb::ErrorData &error) {
	try {
		if (wrapper.stream) {
			auto array = wrapper.stream->Fetch();
			if (!array && wrapper.stream->HasError()) {
				error = wrapper.stream->GetErrorObject();
			}
			return array;
		}
		if (wrapper.result) {
			return wrapper.result->Fetch<duckdb::ArrowFormat>();
		}
	} catch (std::exception &ex) {
		error = duckdb::ErrorData(ex);
	} catch (...) {
		error = duckdb::ErrorData("Unknown error in Fetch");
	}
	return nullptr;
}

//! Drains the stream into `materialized`; an error ends the drain and is recorded on the wrapper
static void MaterializeStream(DuckDBAdbcStreamWrapper &wrapper) {
	const duckdb::lock_guard<duckdb::mutex> guard(wrapper.fetch_mutex);
	if (!wrapper.stream) {
		return;
	}
	try {
		while (!wrapper.last_error) {
			duckdb::ErrorData error;
			auto array = FetchArray(wrapper, error);
			if (error.HasError()) {
				SetStreamError(wrapper, error);
				break;
			}
			if (!array) {
				break;
			}
			wrapper.materialized.push_back(std::move(array));
		}
	} catch (std::exception &ex) {
		SetStreamError(wrapper, duckdb::ErrorData(ex));
	}
	wrapper.stream.reset();
	if (wrapper.conn_wrapper) {
		wrapper.conn_wrapper->FinishExecution(wrapper.statement, wrapper.execution, !wrapper.last_error);
	}
}

static void CopySchema(const ArrowSchema &source, duckdb::ArrowSchemaWrapper &target) {
	if (target.arrow_schema.release) {
		target.arrow_schema.release(&target.arrow_schema);
	}
	if (duckdb_nanoarrow::ArrowSchemaDeepCopy(&source, &target.arrow_schema) != NANOARROW_OK) {
		throw duckdb::OutOfMemoryException("Failed to copy the Arrow schema of the result");
	}
}

//! eager completes the result here, for a driver-built query that must reflect the catalog at the call. rows_affected
//! stays -1, which ADBC defines as unknown, unless the statement reports a changed-row count
static AdbcStatusCode ExecuteArrow(duckdb::PreparedStatementWrapper &prepared, DuckDBAdbcStreamWrapper &out,
                                   bool reads_to_the_end, int64_t &rows_affected, struct AdbcError *error, bool eager) {
	out.stream.reset();
	out.result.reset();
	rows_affected = -1;
	duckdb::ErrorData error_data;
	try {
		duckdb::QueryParameters parameters(duckdb::make_shared_ptr<duckdb::ArrowFormat>(STANDARD_VECTOR_SIZE));
		auto result = prepared.statement->Submit(prepared.values, parameters);
		if (!result->HasError() && !eager &&
		    duckdb::StreamsByDefault(prepared.statement->GetStatementType(), *result, reads_to_the_end)) {
			out.stream = duckdb::make_uniq<duckdb::QueryResultStream<duckdb::ArrowFormat>>(std::move(result));
			CopySchema(out.stream->FormatState().Schema(), out.schema);
			return ADBC_STATUS_OK;
		}
		result->Complete();
		if (!result->HasError()) {
			CopySchema(result->FormatState<duckdb::ArrowFormat>().Schema(), out.schema);
			if (result->GetStatementProperties().return_type == duckdb::StatementReturnType::CHANGED_ROWS) {
				auto &arrays = result->Collection<duckdb::ArrowFormat>();
				auto rows_changed = arrays.empty() ? 0 : duckdb::ArrowFormat::ChangedRows(arrays.front()->arrow_array);
				rows_affected = static_cast<int64_t>(rows_changed);
			}
			out.result = std::move(result);
			return ADBC_STATUS_OK;
		}
		error_data = result->GetErrorObject();
	} catch (std::exception &ex) {
		error_data = duckdb::ErrorData(ex);
	} catch (...) {
		error_data = duckdb::ErrorData("Unknown error in Execute");
	}
	SetError(error, error_data.Message());
	AppendDuckDBErrorDetails(error, duckdb::ErrorTypeToC(error_data.Type()));
	return IsCancellation(out.conn_wrapper, error_data.Type()) ? ADBC_STATUS_CANCELLED : ADBC_STATUS_INVALID_ARGUMENT;
}

static AdbcStatusCode QueryInternal(struct AdbcConnection *connection, struct ArrowArrayStream *out, const char *query,
                                    struct AdbcError *error) {
	AdbcStatement statement = {};

	auto status = StatementNew(connection, &statement, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// A metadata call promises a point-in-time snapshot, so its result must complete before the call returns
	static_cast<DuckDBAdbcStatementWrapper *>(statement.private_data)->eager_result = true;
	status = StatementSetSqlQuery(&statement, query, error);
	if (status == ADBC_STATUS_OK) {
		status = StatementExecuteQuery(&statement, out, nullptr, error);
	}
	StatementRelease(&statement, error);
	return status;
}

struct DuckDBAdbcDatabaseWrapper {
	//! The DuckDB Database Configuration
	duckdb_config config = nullptr;
	//! The DuckDB Database
	duckdb_database database = nullptr;
	//! Path of Disk-Based Database or :memory: database (ADBC "path" option)
	std::string path;
	//! Derived path from ADBC "uri" option (after minimal normalization)
	std::string uri_path;
	bool uri_set = false;
	//! Stores config options for round-tripping via GetOption (DuckDB does not have an API to get config options)
	std::unordered_map<std::string, std::string> config_options;
};

// Helper for the ADBC GetOption buffer convention (two-pass pattern):
// Per the ADBC spec, callers first query the required buffer size, then fetch the value:
//   1. Call with value=nullptr or *length=0 → *length is set to the required size (no data written).
//   2. Call again with a sufficiently sized buffer → value is filled and *length is set.
static AdbcStatusCode GetOptionStringHelper(const char *value_str, char *value, size_t *length,
                                            struct AdbcError *error) {
	if (!length) {
		SetError(error, "Missing length pointer");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	size_t required = std::strlen(value_str) + 1; // include null terminator
	if (*length >= required && value) {
		std::memcpy(value, value_str, required);
	}
	*length = required;
	return ADBC_STATUS_OK;
}

void InitializeADBCError(AdbcError *error) {
	if (!error) {
		return;
	}
	// Only call release for callbacks DuckDB owns. The stream wrapper sets
	// adbc_error.message = last_error (strdup) with release = nullptr; calling
	// delete[] on that would be a double-free and allocator mismatch.
	if (error->release == ::ReleaseError || error->release == ::ReleaseErrorWithDuckDBDetails ||
	    error->release == ::ReleaseStreamErrorDetails) {
		error->release(error);
	}
	error->message = nullptr;
	// Don't set to nullptr, as that indicates that it's invalid
	// Use DuckDB's release callback even for an "empty" error.
	error->release = ::ReleaseError;
	std::memset(error->sqlstate, '\0', sizeof(error->sqlstate));
	error->vendor_code = -1;
}

AdbcStatusCode CheckResult(const duckdb_state &res, AdbcError *error, const char *error_msg) {
	if (!error) {
		// Error should be a non-null pointer
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (res != DuckDBSuccess) {
		SetError(error, error_msg);
		return ADBC_STATUS_INTERNAL;
	}
	return ADBC_STATUS_OK;
}

AdbcStatusCode DatabaseNew(struct AdbcDatabase *database, struct AdbcError *error) {
	if (!database) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	database->private_data = nullptr;
	// you can't malloc a struct with a non-trivial C++ constructor
	// and std::string has a non-trivial constructor. so we need
	// to use new and delete rather than malloc and free.
	auto wrapper = new (std::nothrow) DuckDBAdbcDatabaseWrapper;
	if (!wrapper) {
		SetError(error, "Allocation error");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	database->private_data = wrapper;
	auto res = duckdb_create_config(&wrapper->config);
	return CheckResult(res, error, "Failed to allocate");
}

AdbcStatusCode DatabaseSetOption(struct AdbcDatabase *database, const char *key, const char *value,
                                 struct AdbcError *error) {
	if (!database) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto wrapper = static_cast<DuckDBAdbcDatabaseWrapper *>(database->private_data);
	if (strcmp(key, "path") == 0) {
		wrapper->path = value;
		return ADBC_STATUS_OK;
	}
	if (strcmp(key, ADBC_OPTION_USERNAME) == 0 || strcmp(key, ADBC_OPTION_PASSWORD) == 0) {
		SetError(error, "DuckDB does not support authentication");
		return ADBC_STATUS_NOT_IMPLEMENTED;
	}
	if (strcmp(key, "uri") == 0) {
		std::string file_path;
		if (strncmp(value, "file:", 5) == 0) {
			file_path = std::string(value + 5);
		} else if (strncmp(value, "duckdb:", 7) == 0) {
			file_path = std::string(value + 7);
		} else {
			wrapper->uri_path = value;
			wrapper->uri_set = true;
			return ADBC_STATUS_OK;
		}
		auto suffix_pos = file_path.find_first_of("?#");
		if (suffix_pos != std::string::npos) {
			file_path.erase(suffix_pos);
		}
		if (duckdb::StringUtil::StartsWith(file_path, "//")) {
			auto path_start = file_path.find('/', 2);
			std::string authority =
			    (path_start == std::string::npos) ? file_path.substr(2) : file_path.substr(2, path_start - 2);
			auto authority_lc = duckdb::StringUtil::Lower(authority);
			if (path_start == std::string::npos) {
				// Accept file://foo as a relative path for compatibility (e.g., arrow-adbc recipe driver example).
				file_path = (authority_lc.empty() || authority_lc == "localhost") ? std::string() : authority;
			} else {
				if (!authority_lc.empty() && authority_lc != "localhost") {
					SetError(error, "URI with a non-empty authority is not supported");
					return ADBC_STATUS_INVALID_ARGUMENT;
				}
				file_path = file_path.substr(path_start);
			}
		}
		wrapper->uri_path = std::move(file_path);
		wrapper->uri_set = true;
		return ADBC_STATUS_OK;
	}
	auto res = duckdb_set_config(wrapper->config, key, value);
	if (res == DuckDBSuccess) {
		wrapper->config_options[key] = value;
	}
	return CheckResult(res, error, "Failed to set configuration option");
}

AdbcStatusCode DatabaseInit(struct AdbcDatabase *database, struct AdbcError *error) {
	if (!error) {
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!database) {
		SetError(error, "ADBC Database has an invalid pointer");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	char *errormsg = nullptr;
	// TODO can we set the database path via option, too? Does not look like it...
	auto wrapper = static_cast<DuckDBAdbcDatabaseWrapper *>(database->private_data);
	const auto &db_path = wrapper->uri_set ? wrapper->uri_path : wrapper->path;
	auto res = duckdb_open_ext(db_path.c_str(), &wrapper->database, wrapper->config, &errormsg);
	auto adbc_result = CheckResult(res, error, errormsg);
	if (errormsg) {
		free(errormsg);
	}
	return adbc_result;
}

AdbcStatusCode DatabaseRelease(struct AdbcDatabase *database, struct AdbcError *error) {
	if (database && database->private_data) {
		auto wrapper = static_cast<DuckDBAdbcDatabaseWrapper *>(database->private_data);

		duckdb_close(&wrapper->database);
		duckdb_destroy_config(&wrapper->config);
		delete wrapper;
		database->private_data = nullptr;
	}
	return ADBC_STATUS_OK;
}

// Database Typed Option API (ADBC 1.1.0)
AdbcStatusCode DatabaseGetOption(struct AdbcDatabase *database, const char *key, char *value, size_t *length,
                                 struct AdbcError *error) {
	if (!database || !database->private_data) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcDatabaseWrapper *>(database->private_data);
	if (strcmp(key, "path") == 0) {
		return GetOptionStringHelper(wrapper->path.c_str(), value, length, error);
	}
	if (strcmp(key, ADBC_OPTION_USERNAME) == 0 || strcmp(key, ADBC_OPTION_PASSWORD) == 0) {
		SetError(error, "DuckDB does not support authentication");
		return ADBC_STATUS_NOT_IMPLEMENTED;
	}
	if (strcmp(key, "uri") == 0) {
		if (wrapper->uri_set) {
			return GetOptionStringHelper(wrapper->uri_path.c_str(), value, length, error);
		}
		SetError(error, "Option not found: uri");
		return ADBC_STATUS_NOT_FOUND;
	}
	auto it = wrapper->config_options.find(key);
	if (it != wrapper->config_options.end()) {
		return GetOptionStringHelper(it->second.c_str(), value, length, error);
	}
	auto error_message = std::string("Option not found: ") + key;
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode DatabaseGetOptionBytes(struct AdbcDatabase *database, const char *key, uint8_t *value, size_t *length,
                                      struct AdbcError *error) {
	if (!database || !database->private_data) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode DatabaseGetOptionDouble(struct AdbcDatabase *database, const char *key, double *value,
                                       struct AdbcError *error) {
	if (!database || !database->private_data) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode DatabaseGetOptionInt(struct AdbcDatabase *database, const char *key, int64_t *value,
                                    struct AdbcError *error) {
	if (!database || !database->private_data) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode DatabaseSetOptionBytes(struct AdbcDatabase *database, const char *key, const uint8_t *value,
                                      size_t length, struct AdbcError *error) {
	SetError(error, "SetOptionBytes is not supported for database");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode DatabaseSetOptionInt(struct AdbcDatabase *database, const char *key, int64_t value,
                                    struct AdbcError *error) {
	return DatabaseSetOption(database, key, std::to_string(value).c_str(), error);
}

AdbcStatusCode DatabaseSetOptionDouble(struct AdbcDatabase *database, const char *key, double value,
                                       struct AdbcError *error) {
	return DatabaseSetOption(database, key, std::to_string(value).c_str(), error);
}

AdbcStatusCode ConnectionGetTableSchema(struct AdbcConnection *connection, const char *catalog, const char *db_schema,
                                        const char *table_name, struct ArrowSchema *schema, struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (db_schema == nullptr || strlen(db_schema) == 0) {
		// if schema is not set, we use the default schema
		db_schema = "main";
	}
	if (table_name == nullptr) {
		SetError(error, "AdbcConnectionGetTableSchema: must provide table_name");
		return ADBC_STATUS_INVALID_ARGUMENT;
	} else if (strlen(table_name) == 0) {
		SetError(error, "AdbcConnectionGetTableSchema: must provide table_name");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	ArrowArrayStream arrow_stream;

	std::string query = "SELECT * FROM ";
	if (catalog != nullptr && strlen(catalog) > 0) {
		query += duckdb::SQLIdentifier(catalog) + ".";
	}
	query += duckdb::SQLIdentifier(db_schema) + ".";
	query += duckdb::SQLIdentifier(table_name) + " LIMIT 0;";

	auto success = QueryInternal(connection, &arrow_stream, query.c_str(), error);
	if (success != ADBC_STATUS_OK) {
		return success;
	}
	arrow_stream.get_schema(&arrow_stream, schema);
	arrow_stream.release(&arrow_stream);
	return ADBC_STATUS_OK;
}

AdbcStatusCode ConnectionNew(struct AdbcConnection *connection, struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Missing connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto connection_wrapper = new duckdb::DuckDBAdbcConnectionWrapper();
	connection_wrapper->connection = nullptr;
	connection->private_data = connection_wrapper;
	return ADBC_STATUS_OK;
}

static AdbcStatusCode ExecuteQuery(duckdb::DuckDBAdbcConnectionWrapper &conn_wrapper, const char *query,
                                   struct AdbcError *error) {
	// The connection refuses a statement while one of its streams is open
	conn_wrapper.MaterializeStreams();
	ExecutionProgressGuard progress_guard(&conn_wrapper, nullptr);
	auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper.connection);
	auto res = conn->Query(query);
	if (res->HasError()) {
		auto error_message = "Failed to execute query \"" + std::string(query) + "\": " + res->GetError();
		SetError(error, error_message);
		return ADBC_STATUS_INTERNAL;
	}
	return ADBC_STATUS_OK;
}

//! The engine turns a COMMIT of a transaction that an earlier error invalidated into a rollback that reports success,
//! so that case is reported here. Either way the transaction has ended when this returns.
static AdbcStatusCode CommitTransaction(duckdb::DuckDBAdbcConnectionWrapper &conn_wrapper, struct AdbcError *error) {
	// Reading an open stream into memory can fail, which invalidates the transaction
	conn_wrapper.MaterializeStreams();
	auto &context = *reinterpret_cast<duckdb::Connection *>(conn_wrapper.connection)->context;
	std::string invalidated;
	if (context.transaction.HasActiveTransaction() &&
	    duckdb::ValidChecker::IsInvalidated(context.ActiveTransaction())) {
		invalidated = duckdb::ValidChecker::InvalidatedMessage(context.ActiveTransaction());
	}
	auto status = ExecuteQuery(conn_wrapper, "COMMIT", error);
	if (status == ADBC_STATUS_OK && !invalidated.empty()) {
		SetError(error, "Failed to commit, the transaction was rolled back after an earlier error: " + invalidated);
		return ADBC_STATUS_INVALID_STATE;
	}
	return status;
}

static AdbcStatusCode InternalSetOption(duckdb::DuckDBAdbcConnectionWrapper &conn_wrapper, struct AdbcError *error) {
	auto &conn = *reinterpret_cast<duckdb::Connection *>(conn_wrapper.connection);
	auto &options = conn_wrapper.options;
	// If we got here, the options have already been validated and are acceptable
	for (auto &option : options) {
		if (strcmp(option.first.c_str(), ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
			if (strcmp(option.second.c_str(), ADBC_OPTION_VALUE_ENABLED) == 0) {
				if (conn.HasActiveTransaction()) {
					AdbcStatusCode status = CommitTransaction(conn_wrapper, error);
					if (status != ADBC_STATUS_OK) {
						options.clear();
						return status;
					}
				}
			} else if (strcmp(option.second.c_str(), ADBC_OPTION_VALUE_DISABLED) == 0) {
				if (!conn.HasActiveTransaction()) {
					AdbcStatusCode status = ExecuteQuery(conn_wrapper, "START TRANSACTION", error);
					if (status != ADBC_STATUS_OK) {
						options.clear();
						return status;
					}
				}
			}
		}
	}
	options.clear();
	return ADBC_STATUS_OK;
}

static AdbcStatusCode ConnectionSetOptionCurrentValue(duckdb::DuckDBAdbcConnectionWrapper *conn_wrapper,
                                                      const char *sql_prefix, const char *value,
                                                      struct AdbcError *error) {
	if (!conn_wrapper->connection) {
		SetError(error, "Connection is not initialized");
		return ADBC_STATUS_INVALID_STATE;
	}
	std::string query = sql_prefix + duckdb::SQLIdentifier(value);
	return ExecuteQuery(*conn_wrapper, query.c_str(), error);
}

AdbcStatusCode ConnectionSetOption(struct AdbcConnection *connection, const char *key, const char *value,
                                   struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!value) {
		SetError(error, "Option value must not be NULL");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	std::string key_string = std::string(key);
	std::string key_value = std::string(value);
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
		if (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
			conn_wrapper->options[key_string] = key_value;
		} else if (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
			conn_wrapper->options[key_string] = key_value;
		} else {
			auto error_message = "Invalid connection option value " + std::string(key) + "=" + std::string(value);
			SetError(error, error_message);
			return ADBC_STATUS_INVALID_ARGUMENT;
		}
		if (!conn_wrapper->connection) {
			// If the connection has not yet been initialized, we just return here.
			return ADBC_STATUS_OK;
		}
		return InternalSetOption(*conn_wrapper, error);
	}
	if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0) {
		return ConnectionSetOptionCurrentValue(conn_wrapper, "USE ", value, error);
	}
	if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA) == 0) {
		return ConnectionSetOptionCurrentValue(conn_wrapper, "SET schema = ", value, error);
	}
	// This is an unknown option to the DuckDB driver
	auto error_message =
	    "Unknown connection option " + std::string(key) + "=" + (value ? std::string(value) : "(NULL)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

// Connection Typed Option API (ADBC 1.1.0)
AdbcStatusCode ConnectionGetOption(struct AdbcConnection *connection, const char *key, char *value, size_t *length,
                                   struct AdbcError *error) {
	if (!connection || !connection->private_data) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
		if (conn_wrapper->connection) {
			auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);
			const char *val = conn->IsAutoCommit() ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
			return GetOptionStringHelper(val, value, length, error);
		}
		// Not yet initialized; check pending options, default is "true"
		auto it = conn_wrapper->options.find(ADBC_CONNECTION_OPTION_AUTOCOMMIT);
		const char *val = (it != conn_wrapper->options.end()) ? it->second.c_str() : ADBC_OPTION_VALUE_ENABLED;
		return GetOptionStringHelper(val, value, length, error);
	}
	if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0) {
		if (!conn_wrapper->connection) {
			SetError(error, "Connection is not initialized");
			return ADBC_STATUS_INVALID_STATE;
		}
		ArrowArrayStream stream;
		auto status = QueryInternal(connection, &stream, "SELECT current_database()", error);
		if (status != ADBC_STATUS_OK) {
			return status;
		}
		ArrowArray batch;
		batch.release = nullptr;
		stream.get_next(&stream, &batch);
		if (!batch.release || batch.length < 1 || batch.n_children < 1) {
			if (batch.release) {
				batch.release(&batch);
			}
			stream.release(&stream);
			SetError(error, "Failed to get current catalog");
			return ADBC_STATUS_INTERNAL;
		}
		// Access the first column (VARCHAR): buffers are [validity, offsets, data]
		auto *col = batch.children[0];
		auto offsets = static_cast<const int32_t *>(col->buffers[1]);
		auto data = static_cast<const char *>(col->buffers[2]);
		std::string result(data + offsets[0], static_cast<size_t>(offsets[1] - offsets[0]));
		batch.release(&batch);
		stream.release(&stream);
		return GetOptionStringHelper(result.c_str(), value, length, error);
	}
	if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA) == 0) {
		if (!conn_wrapper->connection) {
			SetError(error, "Connection is not initialized");
			return ADBC_STATUS_INVALID_STATE;
		}
		ArrowArrayStream stream;
		auto status = QueryInternal(connection, &stream, "SELECT current_schema()", error);
		if (status != ADBC_STATUS_OK) {
			return status;
		}
		ArrowArray batch;
		batch.release = nullptr;
		stream.get_next(&stream, &batch);
		if (!batch.release || batch.length < 1 || batch.n_children < 1) {
			if (batch.release) {
				batch.release(&batch);
			}
			stream.release(&stream);
			SetError(error, "Failed to get current schema");
			return ADBC_STATUS_INTERNAL;
		}
		auto *col = batch.children[0];
		auto offsets = static_cast<const int32_t *>(col->buffers[1]);
		auto data = static_cast<const char *>(col->buffers[2]);
		std::string result(data + offsets[0], static_cast<size_t>(offsets[1] - offsets[0]));
		batch.release(&batch);
		stream.release(&stream);
		return GetOptionStringHelper(result.c_str(), value, length, error);
	}
	auto error_message = std::string("Option not found: ") + key;
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode ConnectionGetOptionBytes(struct AdbcConnection *connection, const char *key, uint8_t *value,
                                        size_t *length, struct AdbcError *error) {
	if (!connection || !connection->private_data) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode ConnectionGetOptionDouble(struct AdbcConnection *connection, const char *key, double *value,
                                         struct AdbcError *error) {
	if (!connection || !connection->private_data) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode ConnectionGetOptionInt(struct AdbcConnection *connection, const char *key, int64_t *value,
                                      struct AdbcError *error) {
	if (!connection || !connection->private_data) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
		auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
		if (conn_wrapper->connection) {
			auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);
			*value = conn->IsAutoCommit() ? 1 : 0;
			return ADBC_STATUS_OK;
		}
		auto it = conn_wrapper->options.find(ADBC_CONNECTION_OPTION_AUTOCOMMIT);
		if (it != conn_wrapper->options.end()) {
			*value = (it->second == ADBC_OPTION_VALUE_ENABLED) ? 1 : 0;
		} else {
			*value = 1; // default is autocommit enabled
		}
		return ADBC_STATUS_OK;
	}
	auto error_message = std::string("Option not found: ") + key;
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode ConnectionSetOptionBytes(struct AdbcConnection *connection, const char *key, const uint8_t *value,
                                        size_t length, struct AdbcError *error) {
	SetError(error, "SetOptionBytes is not supported for connection");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode ConnectionSetOptionInt(struct AdbcConnection *connection, const char *key, int64_t value,
                                      struct AdbcError *error) {
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
		const char *str_value = value ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
		return ConnectionSetOption(connection, key, str_value, error);
	}
	return ConnectionSetOption(connection, key, std::to_string(value).c_str(), error);
}

AdbcStatusCode ConnectionSetOptionDouble(struct AdbcConnection *connection, const char *key, double value,
                                         struct AdbcError *error) {
	SetError(error, "SetOptionDouble is not supported for connection");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode ConnectionReadPartition(struct AdbcConnection *connection, const uint8_t *serialized_partition,
                                       size_t serialized_length, struct ArrowArrayStream *out,
                                       struct AdbcError *error) {
	SetError(error, "Read Partitions are not supported in DuckDB");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode StatementExecutePartitions(struct AdbcStatement *statement, struct ArrowSchema *schema,
                                          struct AdbcPartitions *partitions, int64_t *rows_affected,
                                          struct AdbcError *error) {
	SetError(error, "Execute Partitions are not supported in DuckDB");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode ConnectionCommit(struct AdbcConnection *connection, struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);
	if (!conn->HasActiveTransaction()) {
		SetError(error, "No active transaction, cannot commit");
		return ADBC_STATUS_INVALID_STATE;
	}

	AdbcStatusCode status = CommitTransaction(*conn_wrapper, error);
	if (status != ADBC_STATUS_OK && status != ADBC_STATUS_INVALID_STATE) {
		return status;
	}
	// The transaction ended, committed or rolled back, so manual commit mode starts the next one
	auto restarted = ExecuteQuery(*conn_wrapper, "START TRANSACTION", error);
	return status == ADBC_STATUS_OK ? restarted : status;
}

AdbcStatusCode ConnectionRollback(struct AdbcConnection *connection, struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Connection is not set");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);
	if (!conn->HasActiveTransaction()) {
		SetError(error, "No active transaction, cannot rollback");
		return ADBC_STATUS_INVALID_STATE;
	}

	// Reading the open streams into memory first would only keep rows of a transaction that is rolled back
	conn_wrapper->CloseStreams("the stream was closed because its transaction was rolled back");
	AdbcStatusCode status = ExecuteQuery(*conn_wrapper, "ROLLBACK", error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	return ExecuteQuery(*conn_wrapper, "START TRANSACTION", error);
}

AdbcStatusCode ConnectionCancel(struct AdbcConnection *connection, struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Missing connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!connection->private_data) {
		SetError(error, "Connection is invalid");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	if (!conn_wrapper->connection) {
		SetError(error, "Connection is not initialized");
		return ADBC_STATUS_INVALID_STATE;
	}
	conn_wrapper->cancel_requested = true;
	duckdb_interrupt(conn_wrapper->connection);
	return ADBC_STATUS_OK;
}

//===--------------------------------------------------------------------===//
// Manual ArrowArray/ArrowSchema construction helpers
//===--------------------------------------------------------------------===//
// A few ADBC result columns must use Arrow features that DuckDB's Arrow
// export cannot produce: ConnectionGetInfo's `info_value` and
// ConnectionGetStatistics' `statistic_value` both have to be dense unions
// per the ADBC spec, while DuckDB's Arrow export only produces sparse
// unions. Those result batches cannot be produced with QueryInternal;
// instead they are built manually with malloc'd buffers and released
// through a recursive release callback, using the helpers below.

// Recursively frees an ArrowArray tree built by InitManualArrayNode.
static void ReleaseManualArray(struct ArrowArray *array) {
	if (!array || !array->release) {
		return;
	}
	if (array->buffers) {
		for (int64_t i = 0; i < array->n_buffers; i++) {
			if (array->buffers[i]) {
				free(const_cast<void *>(array->buffers[i]));
			}
		}
		free(array->buffers);
	}
	if (array->children) {
		for (int64_t i = 0; i < array->n_children; i++) {
			auto child = array->children[i];
			if (child) {
				if (child->release) {
					child->release(child);
				}
				free(child);
			}
		}
		free(array->children);
	}
	array->release = nullptr;
}

// Zero-initializes an ArrowArray node, allocates its buffer/child pointer
// arrays and installs ReleaseManualArray. Children are zero-initialized;
// releasing the root frees a partially built tree, so callers only need to
// release the root on failure.
static AdbcStatusCode InitManualArrayNode(struct ArrowArray *array, int64_t n_buffers, int64_t n_children,
                                          struct AdbcError *error) {
	memset(array, 0, sizeof(*array));
	array->release = ReleaseManualArray;
	if (n_buffers > 0) {
		array->buffers = static_cast<const void **>(malloc(sizeof(void *) * static_cast<size_t>(n_buffers)));
		if (!array->buffers) {
			SetError(error, "Failed to allocate buffers for result");
			return ADBC_STATUS_INTERNAL;
		}
		memset(array->buffers, 0, sizeof(void *) * static_cast<size_t>(n_buffers));
		array->n_buffers = n_buffers;
	}
	if (n_children > 0) {
		array->children =
		    static_cast<struct ArrowArray **>(malloc(sizeof(struct ArrowArray *) * static_cast<size_t>(n_children)));
		if (!array->children) {
			SetError(error, "Failed to allocate children for result");
			return ADBC_STATUS_INTERNAL;
		}
		memset(array->children, 0, sizeof(struct ArrowArray *) * static_cast<size_t>(n_children));
		array->n_children = n_children;
		for (int64_t i = 0; i < n_children; i++) {
			array->children[i] = static_cast<struct ArrowArray *>(malloc(sizeof(struct ArrowArray)));
			if (!array->children[i]) {
				SetError(error, "Failed to allocate child array for result");
				return ADBC_STATUS_INTERNAL;
			}
			memset(array->children[i], 0, sizeof(struct ArrowArray));
		}
	}
	return ADBC_STATUS_OK;
}

// Allocates a zeroed buffer owned by the array node (freed by the release
// callback). Returns nullptr on allocation failure.
static void *AllocManualBuffer(struct ArrowArray *array, idx_t buffer_idx, size_t size) {
	auto alloc_size = size == 0 ? 1 : size;
	auto buffer = malloc(alloc_size);
	if (buffer) {
		memset(buffer, 0, alloc_size);
		array->buffers[buffer_idx] = buffer;
	}
	return buffer;
}

// Fills a utf8 array node from `values` (no NULL entries).
static AdbcStatusCode BuildManualVarcharArray(struct ArrowArray *array, const duckdb::vector<duckdb::string> &values,
                                              struct AdbcError *error) {
	auto count = values.size();
	auto status = InitManualArrayNode(array, 3, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	array->length = static_cast<int64_t>(count);
	auto offsets = static_cast<int32_t *>(AllocManualBuffer(array, 1, sizeof(int32_t) * (count + 1)));
	size_t total_size = 0;
	for (auto &value : values) {
		total_size += value.size();
	}
	auto data = static_cast<char *>(AllocManualBuffer(array, 2, total_size));
	if (!offsets || !data) {
		SetError(error, "Failed to allocate string buffers for result");
		return ADBC_STATUS_INTERNAL;
	}
	int32_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		offsets[i] = offset;
		memcpy(data + offset, values[i].data(), values[i].size());
		offset += static_cast<int32_t>(values[i].size());
	}
	offsets[count] = offset;
	return ADBC_STATUS_OK;
}

// Fills a utf8 array node where every entry is NULL (used for the table-level
// `column_name` column of GetStatistics).
static AdbcStatusCode BuildManualAllNullVarcharArray(struct ArrowArray *array, idx_t count, struct AdbcError *error) {
	auto status = InitManualArrayNode(array, 3, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	array->length = static_cast<int64_t>(count);
	array->null_count = static_cast<int64_t>(count);
	// All-zero validity bitmap (all NULL), all-zero offsets and an empty data buffer.
	auto validity = AllocManualBuffer(array, 0, (count + 7) / 8);
	auto offsets = AllocManualBuffer(array, 1, sizeof(int32_t) * (count + 1));
	auto data = AllocManualBuffer(array, 2, 0);
	if (!validity || !offsets || !data) {
		SetError(error, "Failed to allocate string buffers for result");
		return ADBC_STATUS_INTERNAL;
	}
	return ADBC_STATUS_OK;
}

#define CHECK_MANUAL_NANOARROW(EXPR)                                                                                   \
	do {                                                                                                               \
		if ((EXPR) != 0) {                                                                                             \
			SetError(error, "Failed to build Arrow schema for result");                                                \
			return ADBC_STATUS_INTERNAL;                                                                               \
		}                                                                                                              \
	} while (0)

// Releases a manually built ArrowSchema/ArrowArray pair unless ownership was
// transferred to a stream (BatchToArrayStream zeroes its inputs on success,
// which makes this a no-op).
struct ManualBatchGuard {
	ManualBatchGuard(struct ArrowSchema &schema_p, struct ArrowArray &array_p) : schema(schema_p), array(array_p) {
	}
	~ManualBatchGuard() {
		if (schema.release) {
			schema.release(&schema);
		}
		if (array.release) {
			array.release(&array);
		}
	}
	struct ArrowSchema &schema;
	struct ArrowArray &array;
};

enum class AdbcInfoCode : uint32_t {
	VENDOR_NAME,
	VENDOR_VERSION,
	DRIVER_NAME,
	DRIVER_VERSION,
	DRIVER_ARROW_VERSION,
	DRIVER_ADBC_VERSION,
	UNRECOGNIZED // always the last entry of the enum
};

static AdbcInfoCode ConvertToInfoCode(uint32_t info_code) {
	switch (info_code) {
	case ADBC_INFO_VENDOR_NAME:
		return AdbcInfoCode::VENDOR_NAME;
	case ADBC_INFO_VENDOR_VERSION:
		return AdbcInfoCode::VENDOR_VERSION;
	case ADBC_INFO_DRIVER_NAME:
		return AdbcInfoCode::DRIVER_NAME;
	case ADBC_INFO_DRIVER_VERSION:
		return AdbcInfoCode::DRIVER_VERSION;
	case ADBC_INFO_DRIVER_ARROW_VERSION:
		return AdbcInfoCode::DRIVER_ARROW_VERSION;
	case ADBC_INFO_DRIVER_ADBC_VERSION:
		return AdbcInfoCode::DRIVER_ADBC_VERSION;
	default:
		return AdbcInfoCode::UNRECOGNIZED;
	}
}

namespace {
// One resolved GetInfo row. Only one of `string_value` / `int64_value` is
// meaningful, selected by `type_id` (dense union member index: 0 for
// string_value, 2 for int64_value -- DuckDB never populates the other
// union members).
struct GetInfoEntry {
	uint32_t info_name;
	int8_t type_id;
	duckdb::string string_value;
	int64_t int64_value;
};
} // namespace

// Builds the GetInfo result schema defined in adbc.h: struct<info_name:
// uint32 not null, info_value: dense union>. `info_value` must be a dense
// union per the spec (format "+ud:0,1,2,3,4,5"); DuckDB only ever populates
// the string_value (0) and int64_value (2) members, but all six are
// declared so consumers can rely on the field names/indices from the spec.
// The caller is responsible for releasing `schema` on failure.
static AdbcStatusCode BuildGetInfoSchema(struct ArrowSchema &schema, struct AdbcError *error) {
	using duckdb_nanoarrow::ArrowSchemaAllocateChildren;
	using duckdb_nanoarrow::ArrowSchemaInit;
	using duckdb_nanoarrow::ArrowSchemaSetFormat;
	using duckdb_nanoarrow::ArrowSchemaSetName;

	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(&schema, duckdb_nanoarrow::NANOARROW_TYPE_STRUCT));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(&schema, 2));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(schema.children[0], duckdb_nanoarrow::NANOARROW_TYPE_UINT32));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(schema.children[0], "info_name"));
	schema.children[0]->flags &= ~ARROW_FLAG_NULLABLE;

	// info_value: dense union (the bundled nanoarrow has no union type
	// template, so the format string is set directly).
	auto value_schema = schema.children[1];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema, duckdb_nanoarrow::NANOARROW_TYPE_UNINITIALIZED));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetFormat(value_schema, "+ud:0,1,2,3,4,5"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema, "info_value"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(value_schema, 6));

	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[0], "string_value"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[1], duckdb_nanoarrow::NANOARROW_TYPE_BOOL));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[1], "bool_value"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[2], duckdb_nanoarrow::NANOARROW_TYPE_INT64));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[2], "int64_value"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[3], duckdb_nanoarrow::NANOARROW_TYPE_INT32));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[3], "int32_bitmask"));

	// string_list: list<utf8>.
	auto list_schema = value_schema->children[4];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(list_schema, duckdb_nanoarrow::NANOARROW_TYPE_LIST));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(list_schema, "string_list"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(list_schema, 1));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(list_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(list_schema->children[0], "item"));

	// int32_to_int32_list_map: map<int32, list<int32>>, encoded as
	// list<struct<key: int32 not null, value: list<int32>>> per the Arrow map
	// layout.
	auto map_schema = value_schema->children[5];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(map_schema, duckdb_nanoarrow::NANOARROW_TYPE_MAP));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(map_schema, "int32_to_int32_list_map"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(map_schema, 1));
	auto entries_schema = map_schema->children[0];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(entries_schema, duckdb_nanoarrow::NANOARROW_TYPE_STRUCT));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(entries_schema, "entries"));
	entries_schema->flags &= ~ARROW_FLAG_NULLABLE;
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(entries_schema, 2));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(entries_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_INT32));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(entries_schema->children[0], "key"));
	entries_schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
	auto map_value_schema = entries_schema->children[1];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(map_value_schema, duckdb_nanoarrow::NANOARROW_TYPE_LIST));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(map_value_schema, "value"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(map_value_schema, 1));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(map_value_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_INT32));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(map_value_schema->children[0], "item"));

	return ADBC_STATUS_OK;
}

// Assembles the GetInfo batch from resolved entries. The caller releases
// `array` on failure.
static AdbcStatusCode BuildGetInfoArray(struct ArrowArray &array, const duckdb::vector<GetInfoEntry> &entries,
                                        struct AdbcError *error) {
	auto count = entries.size();

	// Root struct: one row per requested (and recognized) info code.
	auto status = InitManualArrayNode(&array, 1, 2, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	array.length = static_cast<int64_t>(count);

	// info_name.
	auto name_array = array.children[0];
	status = InitManualArrayNode(name_array, 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	name_array->length = static_cast<int64_t>(count);
	auto names = static_cast<uint32_t *>(AllocManualBuffer(name_array, 1, sizeof(uint32_t) * count));
	if (!names) {
		SetError(error, "Failed to allocate name buffer for GetInfo result");
		return ADBC_STATUS_INTERNAL;
	}
	for (idx_t i = 0; i < count; i++) {
		names[i] = entries[i].info_name;
	}

	// info_value: dense union. Only members 0 (string_value) and 2
	// (int64_value) are ever populated by DuckDB; the rest are always empty.
	auto value_array = array.children[1];
	status = InitManualArrayNode(value_array, 2, 6, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	value_array->length = static_cast<int64_t>(count);
	auto type_ids = static_cast<int8_t *>(AllocManualBuffer(value_array, 0, count));
	auto value_offsets = static_cast<int32_t *>(AllocManualBuffer(value_array, 1, sizeof(int32_t) * count));
	if (!type_ids || !value_offsets) {
		SetError(error, "Failed to allocate union buffers for GetInfo result");
		return ADBC_STATUS_INTERNAL;
	}
	duckdb::vector<duckdb::string> string_values;
	duckdb::vector<int64_t> int64_values;
	for (idx_t i = 0; i < count; i++) {
		auto &entry = entries[i];
		type_ids[i] = entry.type_id;
		switch (entry.type_id) {
		case 0:
			value_offsets[i] = static_cast<int32_t>(string_values.size());
			string_values.push_back(entry.string_value);
			break;
		case 2:
			value_offsets[i] = static_cast<int32_t>(int64_values.size());
			int64_values.push_back(entry.int64_value);
			break;
		default:
			// Unreachable: ConnectionGetInfo only ever produces type_id 0 or 2.
			SetError(error, "Unexpected GetInfo union type id");
			return ADBC_STATUS_INTERNAL;
		}
	}

	// string_value: populated.
	status = BuildManualVarcharArray(value_array->children[0], string_values, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// bool_value: always empty.
	status = InitManualArrayNode(value_array->children[1], 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// int64_value: populated.
	auto int64_child = value_array->children[2];
	status = InitManualArrayNode(int64_child, 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	int64_child->length = static_cast<int64_t>(int64_values.size());
	if (!int64_values.empty()) {
		auto int64_data =
		    static_cast<int64_t *>(AllocManualBuffer(int64_child, 1, sizeof(int64_t) * int64_values.size()));
		if (!int64_data) {
			SetError(error, "Failed to allocate union buffers for GetInfo result");
			return ADBC_STATUS_INTERNAL;
		}
		for (idx_t i = 0; i < int64_values.size(); i++) {
			int64_data[i] = int64_values[i];
		}
	}
	// int32_bitmask: always empty.
	status = InitManualArrayNode(value_array->children[3], 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// string_list: always empty, but variable-size layouts need an offsets
	// buffer with a single zero entry (see BuildGetStatisticsArray's
	// binary_child for the same requirement).
	auto list_child = value_array->children[4];
	status = InitManualArrayNode(list_child, 2, 1, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	if (!AllocManualBuffer(list_child, 1, sizeof(int32_t))) {
		SetError(error, "Failed to allocate union buffers for GetInfo result");
		return ADBC_STATUS_INTERNAL;
	}
	status = BuildManualVarcharArray(list_child->children[0], duckdb::vector<duckdb::string>(), error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// int32_to_int32_list_map: always empty; same offsets requirement as
	// string_list, plus an empty `entries` struct (key int32 / value
	// list<int32>) beneath it.
	auto map_child = value_array->children[5];
	status = InitManualArrayNode(map_child, 2, 1, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	if (!AllocManualBuffer(map_child, 1, sizeof(int32_t))) {
		SetError(error, "Failed to allocate union buffers for GetInfo result");
		return ADBC_STATUS_INTERNAL;
	}
	auto entries_child = map_child->children[0];
	status = InitManualArrayNode(entries_child, 1, 2, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = InitManualArrayNode(entries_child->children[0], 2, 0, error); // key
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	auto map_value_child = entries_child->children[1]; // value: list<int32>
	status = InitManualArrayNode(map_value_child, 2, 1, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	if (!AllocManualBuffer(map_value_child, 1, sizeof(int32_t))) {
		SetError(error, "Failed to allocate union buffers for GetInfo result");
		return ADBC_STATUS_INTERNAL;
	}
	status = InitManualArrayNode(map_value_child->children[0], 2, 0, error); // item
	if (status != ADBC_STATUS_OK) {
		return status;
	}

	return ADBC_STATUS_OK;
}

AdbcStatusCode ConnectionGetInfo(struct AdbcConnection *connection, const uint32_t *info_codes,
                                 size_t info_codes_length, struct ArrowArrayStream *out, struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Missing connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!connection->private_data) {
		SetError(error, "Connection is invalid");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!out) {
		SetError(error, "Output parameter was not provided");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	if (!conn_wrapper->connection) {
		SetError(error, "Connection is not initialized");
		return ADBC_STATUS_INVALID_STATE;
	}

	// If 'info_codes' is NULL, we should output all the info codes we recognize
	static constexpr uint32_t DEFAULT_INFO_CODES[] = {ADBC_INFO_VENDOR_NAME,          ADBC_INFO_VENDOR_VERSION,
	                                                  ADBC_INFO_DRIVER_NAME,          ADBC_INFO_DRIVER_VERSION,
	                                                  ADBC_INFO_DRIVER_ARROW_VERSION, ADBC_INFO_DRIVER_ADBC_VERSION};
	const uint32_t *requested_codes = info_codes ? info_codes : DEFAULT_INFO_CODES;
	size_t length = info_codes ? info_codes_length : (sizeof(DEFAULT_INFO_CODES) / sizeof(DEFAULT_INFO_CODES[0]));

	// The info_value column is a dense union per the ADBC spec, which
	// DuckDB's Arrow export cannot produce; the batch is built manually
	// below instead of going through QueryInternal. Resolve the requested
	// codes into entries first, ignoring unrecognized codes.
	duckdb::vector<GetInfoEntry> entries;
	for (size_t i = 0; i < length; i++) {
		auto code = duckdb::NumericCast<uint32_t>(requested_codes[i]);
		auto info_code = ConvertToInfoCode(code);
		switch (info_code) {
		case AdbcInfoCode::VENDOR_NAME:
			entries.push_back({(uint32_t)ADBC_INFO_VENDOR_NAME, 0, "duckdb", 0});
			break;
		case AdbcInfoCode::VENDOR_VERSION:
			entries.push_back({(uint32_t)ADBC_INFO_VENDOR_VERSION, 0, duckdb_library_version(), 0});
			break;
		case AdbcInfoCode::DRIVER_NAME:
			entries.push_back({(uint32_t)ADBC_INFO_DRIVER_NAME, 0, "ADBC DuckDB Driver", 0});
			break;
		case AdbcInfoCode::DRIVER_VERSION:
			entries.push_back({(uint32_t)ADBC_INFO_DRIVER_VERSION, 0, duckdb_library_version(), 0});
			break;
		case AdbcInfoCode::DRIVER_ARROW_VERSION:
			// TODO: fill in arrow version
			entries.push_back({(uint32_t)ADBC_INFO_DRIVER_ARROW_VERSION, 0, "(unknown)", 0});
			break;
		case AdbcInfoCode::DRIVER_ADBC_VERSION:
			entries.push_back(
			    {(uint32_t)ADBC_INFO_DRIVER_ADBC_VERSION, 2, duckdb::string(), (int64_t)ADBC_VERSION_1_1_0});
			break;
		case AdbcInfoCode::UNRECOGNIZED:
			// Unrecognized codes are not an error, just ignored
			continue;
		default:
			// Codes that we have implemented but not handled here are a developer error
			SetError(error, "Info code recognized but not handled");
			return ADBC_STATUS_INTERNAL;
		}
	}

	struct ArrowSchema schema;
	memset(&schema, 0, sizeof(schema));
	struct ArrowArray array;
	memset(&array, 0, sizeof(array));
	ManualBatchGuard guard(schema, array);

	auto status = BuildGetInfoSchema(schema, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = BuildGetInfoArray(array, entries, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// Per ADBC semantics `out` is a pure output parameter that may contain
	// uninitialized stack garbage, so it must be zeroed rather than letting
	// BatchToArrayStream inspect `out->release`; see ConnectionGetStatistics.
	memset(out, 0, sizeof(*out));
	// On success ownership of schema/array moves into the stream and the guard
	// becomes a no-op.
	return BatchToArrayStream(&array, &schema, out, error);
}

AdbcStatusCode ConnectionInit(struct AdbcConnection *connection, struct AdbcDatabase *database,
                              struct AdbcError *error) {
	if (!database) {
		SetError(error, "Missing database object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!database->private_data) {
		SetError(error, "Invalid database");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!connection) {
		SetError(error, "Missing connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto database_wrapper = static_cast<DuckDBAdbcDatabaseWrapper *>(database->private_data);
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	conn_wrapper->connection = nullptr;

	auto res = duckdb_connect(database_wrapper->database, &conn_wrapper->connection);
	auto adbc_status = CheckResult(res, error, "Failed to connect to Database");
	if (adbc_status != ADBC_STATUS_OK) {
		return adbc_status;
	}
	auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);
	// Track query progress for ADBC without printing a progress bar.
	auto &client_config = duckdb::ClientConfig::GetConfig(*conn->context);
	if (!client_config.system_progress_bar_disable_reason) {
		client_config.enable_progress_bar = true;
		client_config.print_progress_bar = false;
	}
	// We might have options to set
	return InternalSetOption(*conn_wrapper, error);
}

AdbcStatusCode ConnectionRelease(struct AdbcConnection *connection, struct AdbcError *error) {
	if (connection && connection->private_data) {
		auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
		// Materialize active streams before disconnecting so they remain readable
		conn_wrapper->MaterializeStreams();
		// Detach active streams and statements before deleting conn_wrapper to avoid dangling pointers
		conn_wrapper->DetachAndClearStreams();
		conn_wrapper->DetachAndClearStatements();
		auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);
		duckdb_disconnect(reinterpret_cast<duckdb_connection *>(&conn));
		delete conn_wrapper;
		connection->private_data = nullptr;
	}
	return ADBC_STATUS_OK;
}

// some stream callbacks

static int get_schema(struct ArrowArrayStream *stream, struct ArrowSchema *out) {
	if (!stream || !stream->private_data || !out) {
		return DuckDBError;
	}
	auto &result_wrapper = *static_cast<DuckDBAdbcStreamWrapper *>(stream->private_data);
	if (duckdb_nanoarrow::ArrowSchemaDeepCopy(&result_wrapper.schema.arrow_schema, out) != NANOARROW_OK) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

static int get_next(struct ArrowArrayStream *stream, struct ArrowArray *out) {
	if (!stream || !stream->private_data || !out) {
		return DuckDBError;
	}
	out->release = nullptr;
	auto &result_wrapper = *static_cast<DuckDBAdbcStreamWrapper *>(stream->private_data);
	// A statement on the same connection drains or closes this stream from its own thread; serialize with it
	const duckdb::lock_guard<duckdb::mutex> guard(result_wrapper.fetch_mutex);
	if (result_wrapper.materialized_index < result_wrapper.materialized.size()) {
		auto array = std::move(result_wrapper.materialized[result_wrapper.materialized_index++]);
		array->MoveTo(*out);
		return DuckDBSuccess;
	}
	if (result_wrapper.last_error) {
		return DuckDBError;
	}
	duckdb::ErrorData error;
	auto array = FetchArray(result_wrapper, error);
	if ((!array || error.HasError()) && result_wrapper.conn_wrapper) {
		result_wrapper.conn_wrapper->FinishExecution(result_wrapper.statement, result_wrapper.execution,
		                                             !error.HasError());
	}
	if (error.HasError()) {
		SetStreamError(result_wrapper, error);
		return DuckDBError;
	}
	if (array) {
		array->MoveTo(*out);
	}
	return DuckDBSuccess;
}

void release(struct ArrowArrayStream *stream) {
	if (!stream || !stream->release) {
		return;
	}
	auto result_wrapper = static_cast<DuckDBAdbcStreamWrapper *>(stream->private_data);
	if (result_wrapper) {
		if (result_wrapper->conn_wrapper) {
			result_wrapper->conn_wrapper->UnregisterStream(result_wrapper);
			// Ends this stream's execution if it has not ended already: a result released before it was read out did
			// not complete. A later execution of the same statement keeps its progress.
			result_wrapper->conn_wrapper->FinishExecution(result_wrapper->statement, result_wrapper->execution, false);
		}
		delete result_wrapper;
	}
	stream->private_data = nullptr;
	stream->release = nullptr;
}

const char *get_last_error(struct ArrowArrayStream *stream) {
	if (!stream || !stream->private_data) {
		return nullptr;
	}
	auto result_wrapper = reinterpret_cast<DuckDBAdbcStreamWrapper *>(stream->private_data);
	const duckdb::lock_guard<duckdb::mutex> guard(result_wrapper->fetch_mutex);
	return result_wrapper->last_error;
}

const AdbcError *ErrorFromArrayStream(struct ArrowArrayStream *stream, AdbcStatusCode *status) {
	if (!stream || !stream->private_data) {
		return nullptr;
	}
	// Verify the stream comes from this driver by checking the release function
	if (stream->release != release) {
		return nullptr;
	}
	auto result_wrapper = reinterpret_cast<DuckDBAdbcStreamWrapper *>(stream->private_data);
	const duckdb::lock_guard<duckdb::mutex> guard(result_wrapper->fetch_mutex);
	if (!result_wrapper->last_error) {
		return nullptr;
	}
	if (status) {
		*status = result_wrapper->status_code;
	}
	return &result_wrapper->adbc_error;
}

// this is an evil hack, normally we would need a stream factory here, but its probably much easier if the adbc clients
// just hand over a stream

duckdb::unique_ptr<duckdb::ArrowArrayStreamWrapper> stream_produce(uintptr_t factory_ptr,
                                                                   duckdb::ArrowStreamParameters &parameters) {
	// TODO this will ignore any projections or filters but since we don't expose the scan it should be sort of fine
	auto res = duckdb::make_uniq<duckdb::ArrowArrayStreamWrapper>();
	res->arrow_array_stream = *reinterpret_cast<ArrowArrayStream *>(factory_ptr);
	return res;
}

void stream_schema(ArrowArrayStream *stream, ArrowSchema &schema) {
	stream->get_schema(stream, &schema);
}

// Helper function to build CREATE TABLE SQL statement
static std::string BuildCreateTableSQL(const char *catalog, const char *schema, const char *table_name,
                                       const duckdb::vector<duckdb::LogicalType> &types,
                                       const duckdb::vector<std::string> &names, bool if_not_exists = false,
                                       bool temporary = false, bool replace = false) {
	std::ostringstream create_table;
	if (replace) {
		create_table << "CREATE OR REPLACE ";
	} else {
		create_table << "CREATE ";
	}
	if (temporary) {
		create_table << "TEMP ";
	}
	create_table << "TABLE ";
	if (if_not_exists) {
		create_table << "IF NOT EXISTS ";
	}
	// Note: DuckDB resolves two-part names as either catalog.table (default schema)
	// or schema.table depending on context. This can become ambiguous if a schema and
	// an attached catalog share a name. Callers should prefer passing an explicit
	// schema (or defaulting to "main") to produce an unambiguous three-part name.
	// For TEMP tables, specifying catalog/schema in the CREATE statement is not allowed;
	// the table is automatically placed in the temp catalog.
	if (!temporary) {
		if (catalog) {
			create_table << duckdb::SQLIdentifier(catalog) << ".";
		}
		if (schema) {
			create_table << duckdb::SQLIdentifier(schema) << ".";
		}
	}
	create_table << duckdb::SQLIdentifier(table_name) << " (";
	for (idx_t i = 0; i < types.size(); i++) {
		create_table << duckdb::SQLIdentifier(names[i]);
		create_table << " " << types[i].ToString();
		if (i + 1 < types.size()) {
			create_table << ", ";
		}
	}
	create_table << ");";
	return create_table.str();
}

AdbcStatusCode Ingest(duckdb_connection connection,
                      duckdb::optional_ptr<duckdb::DuckDBAdbcConnectionWrapper> conn_wrapper, const char *catalog,
                      const char *table_name, const char *schema, struct ArrowArrayStream *input,
                      struct AdbcError *error, IngestionMode ingestion_mode, bool temporary, int64_t *rows_affected) {
	if (!connection) {
		SetError(error, "Missing connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!input) {
		SetError(error, "Missing input arrow stream pointer");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!table_name) {
		SetError(error, "Missing database object name");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	const auto missing_table_error = std::string("Table \"") + table_name + "\" does not exist";
	auto set_ingest_error = [&](const std::string &msg) {
		if (msg.find("could not be found") != std::string::npos) {
			SetError(error, missing_table_error);
		} else {
			SetError(error, msg);
		}
	};
	if (schema && temporary) {
		// Temporary option is not supported with ADBC_INGEST_OPTION_TARGET_DB_SCHEMA
		SetError(error, "Temporary option is not supported with schema");
		return ADBC_STATUS_INVALID_STATE;
	}
	if (catalog && temporary) {
		// Temporary option is not supported with ADBC_INGEST_OPTION_TARGET_CATALOG
		SetError(error, "Temporary option is not supported with catalog");
		return ADBC_STATUS_INVALID_STATE;
	}

	// Resolve target name parts.
	// Used for both SQL generation (CREATE/DROP) and appender lookup.
	// Prefer explicit three-part names; two-part names can be ambiguous.
	const char *effective_catalog = catalog;
	const char *effective_schema = schema;
	std::string resolved_catalog;
	if (temporary) {
		// Temporary tables live in the special "temp" catalog.
		// "CREATE TEMP TABLE" automatically places tables in temp.main.
		// For the appender, we need to explicitly target the temp catalog.
		effective_catalog = "temp";
		effective_schema = nullptr;
	} else if (catalog && !schema) {
		// Default schema for attached catalogs (DEFAULT_SCHEMA).
		// Use catalog.main.table to avoid catalog/schema name ambiguity.
		effective_schema = "main";
	} else if (!catalog) {
		// DuckDB's name resolution prioritizes the "temp" catalog, so without an explicit
		// catalog a same-named temp table would shadow the persistent target. Resolve the
		// current catalog explicitly to avoid that. Fallback "memory" may not match the
		// actual catalog for file-based databases.
		duckdb_result cat_result = {};
		if (duckdb_query(connection, "SELECT current_catalog()", &cat_result) == DuckDBSuccess) {
			char *val = duckdb_value_varchar(&cat_result, 0, 0);
			if (val) {
				resolved_catalog = val;
				duckdb_free(val);
			}
			effective_catalog = !resolved_catalog.empty() ? resolved_catalog.c_str() : "memory";
		} else {
			effective_catalog = "memory";
		}
		duckdb_destroy_result(&cat_result);
		if (!schema) {
			effective_schema = "main";
		}
	}

	duckdb::ArrowSchemaWrapper arrow_schema_wrapper;
	ConvertedSchemaWrapper out_types;

	input->get_schema(input, &arrow_schema_wrapper.arrow_schema);
	auto res = duckdb_schema_from_arrow(connection, &arrow_schema_wrapper.arrow_schema, out_types.GetPtr());
	if (res) {
		SetError(error, duckdb_error_data_message(res));
		AppendDuckDBErrorDetails(error, res);
		duckdb_destroy_error_data(&res);
		return ADBC_STATUS_INTERNAL;
	}

	auto &d_converted_schema = *reinterpret_cast<duckdb::ArrowTableSchema *>(out_types.Get());
	auto types = d_converted_schema.GetTypes();
	auto names = d_converted_schema.GetNames();

	// Handle different ingestion modes
	switch (ingestion_mode) {
	case IngestionMode::CREATE: {
		// CREATE mode: Create table, error if already exists
		auto sql = BuildCreateTableSQL(effective_catalog, effective_schema, table_name, types, names, false, temporary);
		duckdb_result result;
		if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError) {
			const char *error_msg = duckdb_result_error(&result);
			bool already_exists = error_msg && std::string(error_msg).find("already exists") != std::string::npos;
			bool interrupted = IsCancellation(conn_wrapper, duckdb::ErrorTypeFromC(duckdb_result_error_type(&result)));
			SetError(error, error_msg);
			AppendDuckDBErrorDetails(error, duckdb_result_error_type(&result));
			duckdb_destroy_result(&result);
			if (interrupted) {
				return ADBC_STATUS_CANCELLED;
			}
			if (already_exists) {
				return ADBC_STATUS_ALREADY_EXISTS;
			}
			return ADBC_STATUS_INTERNAL;
		}
		duckdb_destroy_result(&result);
		break;
	}
	case IngestionMode::APPEND:
		// APPEND mode: No pre-check needed
		// The appender will naturally fail if the table doesn't exist
		break;
	case IngestionMode::REPLACE: {
		// REPLACE mode: CREATE OR REPLACE TABLE
		auto create_sql =
		    BuildCreateTableSQL(effective_catalog, effective_schema, table_name, types, names, false, temporary, true);
		duckdb_result result;
		if (duckdb_query(connection, create_sql.c_str(), &result) == DuckDBError) {
			auto err = duckdb_result_error(&result);
			SetError(error, err);
			AppendDuckDBErrorDetails(error, duckdb_result_error_type(&result));
			bool interrupted = IsCancellation(conn_wrapper, duckdb::ErrorTypeFromC(duckdb_result_error_type(&result)));
			duckdb_destroy_result(&result);
			return interrupted ? ADBC_STATUS_CANCELLED : ADBC_STATUS_INTERNAL;
		}
		duckdb_destroy_result(&result);
		break;
	}
	case IngestionMode::CREATE_APPEND: {
		// CREATE_APPEND mode: Create if not exists, append if exists
		auto sql = BuildCreateTableSQL(effective_catalog, effective_schema, table_name, types, names, true, temporary);
		duckdb_result result;
		if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError) {
			auto err = duckdb_result_error(&result);
			SetError(error, err);
			AppendDuckDBErrorDetails(error, duckdb_result_error_type(&result));
			bool interrupted = IsCancellation(conn_wrapper, duckdb::ErrorTypeFromC(duckdb_result_error_type(&result)));
			duckdb_destroy_result(&result);
			return interrupted ? ADBC_STATUS_CANCELLED : ADBC_STATUS_INTERNAL;
		}
		duckdb_destroy_result(&result);
		break;
	}
	}
	AppenderWrapper appender(connection, effective_catalog, effective_schema, table_name);
	if (!appender.Valid()) {
		if (!appender.CreateError().empty()) {
			set_ingest_error(appender.CreateError());
			AppendDuckDBErrorDetails(error, appender.CreateErrorType());
		} else {
			SetError(error, missing_table_error);
		}
		return ADBC_STATUS_INTERNAL;
	}
	auto appender_error = [&]() {
		auto error_data = duckdb_appender_error_data(appender.Get());
		auto err = duckdb_error_data_message(error_data);
		if (err && err[0] != '\0') {
			set_ingest_error(err);
		} else {
			SetError(error, missing_table_error);
		}
		bool interrupted =
		    IsCancellation(conn_wrapper, duckdb::ErrorTypeFromC(duckdb_error_data_error_type(error_data)));
		AppendDuckDBErrorDetails(error, error_data);
		duckdb_destroy_error_data(&error_data);
		return interrupted ? ADBC_STATUS_CANCELLED : ADBC_STATUS_INTERNAL;
	};
	duckdb::ArrowArrayWrapper arrow_array_wrapper;

	// Initialize rows_affected counter if requested
	int64_t affected = 0;

	input->get_next(input, &arrow_array_wrapper.arrow_array);
	while (arrow_array_wrapper.arrow_array.release) {
		DataChunkWrapper out_chunk;
		auto res = duckdb_data_chunk_from_arrow(connection, &arrow_array_wrapper.arrow_array, out_types.Get(),
		                                        &out_chunk.chunk);
		if (res) {
			SetError(error, duckdb_error_data_message(res));
			AppendDuckDBErrorDetails(error, res);
			duckdb_destroy_error_data(&res);
		}
		// Count rows for rows_affected, if a chunk was produced
		if (out_chunk.chunk) {
			auto *chunk = reinterpret_cast<duckdb::DataChunk *>(out_chunk.chunk);
			affected += static_cast<int64_t>(chunk->size());
		}
		if (duckdb_append_data_chunk(appender.Get(), out_chunk.chunk) != DuckDBSuccess) {
			return appender_error();
		}
		arrow_array_wrapper = duckdb::ArrowArrayWrapper();
		input->get_next(input, &arrow_array_wrapper.arrow_array);
	}
	// The appender writes the rows it still holds when it closes, so a failure there must fail the ingestion
	if (duckdb_appender_close(appender.Get()) != DuckDBSuccess) {
		return appender_error();
	}
	if (rows_affected) {
		*rows_affected = affected;
	}
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementNew(struct AdbcConnection *connection, struct AdbcStatement *statement,
                            struct AdbcError *error) {
	if (!connection) {
		SetError(error, "Missing connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!connection->private_data) {
		SetError(error, "Invalid connection object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	statement->private_data = nullptr;

	auto statement_wrapper = new (std::nothrow) DuckDBAdbcStatementWrapper();
	if (!statement_wrapper) {
		SetError(error, "Allocation error");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	statement->private_data = statement_wrapper;
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);

	statement_wrapper->connection = conn_wrapper->connection;
	statement_wrapper->conn_wrapper = conn_wrapper;
	statement_wrapper->statement = nullptr;
	statement_wrapper->ingestion_stream.release = nullptr;
	statement_wrapper->ingestion_table_name = nullptr;
	statement_wrapper->target_catalog = nullptr;
	statement_wrapper->db_schema = nullptr;
	statement_wrapper->temporary_table = false;
	statement_wrapper->eager_result = false;

	statement_wrapper->ingestion_mode = IngestionMode::CREATE;
	conn_wrapper->RegisterStatement(statement_wrapper);
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementRelease(struct AdbcStatement *statement, struct AdbcError *error) {
	if (!statement || !statement->private_data) {
		return ADBC_STATUS_OK;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (wrapper->statement) {
		duckdb_destroy_prepare(&wrapper->statement);
		wrapper->statement = nullptr;
	}
	if (wrapper->ingestion_stream.release) {
		wrapper->ingestion_stream.release(&wrapper->ingestion_stream);
		wrapper->ingestion_stream.release = nullptr;
	}
	if (wrapper->ingestion_table_name) {
		free(wrapper->ingestion_table_name);
		wrapper->ingestion_table_name = nullptr;
	}
	if (wrapper->target_catalog) {
		free(wrapper->target_catalog);
		wrapper->target_catalog = nullptr;
	}
	if (wrapper->db_schema) {
		free(wrapper->db_schema);
		wrapper->db_schema = nullptr;
	}
	if (wrapper->conn_wrapper && !wrapper->connection_released) {
		wrapper->conn_wrapper->UnregisterStatement(wrapper);
	}
	delete wrapper;
	statement->private_data = nullptr;
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementCancel(struct AdbcStatement *statement, struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (!wrapper->connection) {
		// Statement has been released or is not properly initialized.
		// Return INVALID_ARGUMENT since the statement object itself is invalid.
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (wrapper->conn_wrapper) {
		wrapper->conn_wrapper->cancel_requested = true;
	}
	duckdb_interrupt(wrapper->connection);
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementExecuteSchema(struct AdbcStatement *statement, struct ArrowSchema *schema,
                                      struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!schema) {
		SetError(error, "Missing schema object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (!wrapper->statement) {
		SetError(error, "Must call StatementSetSqlQuery before StatementExecuteSchema");
		return ADBC_STATUS_INVALID_STATE;
	}

	if (wrapper->conn_wrapper) {
		wrapper->conn_wrapper->MaterializeStreams();
	}

	auto &prepared = *reinterpret_cast<duckdb::PreparedStatementWrapper *>(wrapper->statement);
	if (prepared.statement && !prepared.statement->HasError() && !prepared.statement->MetadataAvailable()) {
		SetError(error, "The result schema of this statement is known only once it is executed: it expands into "
		                "several statements, or the types of its parameters are not known yet");
		return ADBC_STATUS_NOT_IMPLEMENTED;
	}

	auto count = duckdb_prepared_statement_column_count(wrapper->statement);
	std::vector<duckdb_logical_type> types(count);
	std::vector<std::string> owned_names;
	owned_names.reserve(count);
	duckdb::vector<const char *> names(count);

	for (idx_t i = 0; i < count; i++) {
		types[i] = duckdb_prepared_statement_column_logical_type(wrapper->statement, i);
		auto column_name = duckdb_prepared_statement_column_name(wrapper->statement, i);
		owned_names.emplace_back(column_name ? column_name : "");
		names[i] = owned_names.back().c_str();
		duckdb_free(const_cast<char *>(column_name));
	}

	duckdb_arrow_options arrow_options;
	duckdb_connection_get_arrow_options(wrapper->connection, &arrow_options);

	auto res = duckdb_to_arrow_schema(arrow_options, types.data(), names.data(), count, schema);

	for (auto &type : types) {
		duckdb_destroy_logical_type(&type);
	}
	duckdb_destroy_arrow_options(&arrow_options);

	if (res) {
		SetError(error, duckdb_error_data_message(res));
		AppendDuckDBErrorDetails(error, res);
		duckdb_destroy_error_data(&res);
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementGetParameterSchema(struct AdbcStatement *statement, struct ArrowSchema *schema,
                                           struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!schema) {
		SetError(error, "Missing schema object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	// TODO: we might want to cache this, but then we need to return a deep copy anyways.., so I'm not sure if that
	// would be worth the extra management

	auto prepared_wrapper = reinterpret_cast<duckdb::PreparedStatementWrapper *>(wrapper->statement);
	if (!prepared_wrapper || !prepared_wrapper->statement) {
		SetError(error, "Invalid prepared statement wrapper");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto count = prepared_wrapper->statement->GetParameterCount();
	std::vector<duckdb_logical_type> types(count);
	std::vector<std::string> owned_names;
	owned_names.reserve(count);
	duckdb::vector<const char *> names(count);

	for (idx_t i = 0; i < count; i++) {
		// FIXME: we don't support named parameters yet, but when we do, this needs to be updated
		// Every prepared parameter type is UNKNOWN, which we need to map to NULL according to the spec of
		// 'AdbcStatementGetParameterSchema'
		types[i] = duckdb_create_logical_type(DUCKDB_TYPE_SQLNULL);
		auto column_name = std::to_string(i);
		owned_names.emplace_back(column_name);
		names[i] = owned_names.back().c_str();
	}

	duckdb_arrow_options arrow_options;
	duckdb_connection_get_arrow_options(wrapper->connection, &arrow_options);

	auto res = duckdb_to_arrow_schema(arrow_options, types.data(), names.data(), count, schema);

	for (auto &type : types) {
		duckdb_destroy_logical_type(&type);
	}
	duckdb_destroy_arrow_options(&arrow_options);

	if (res) {
		SetError(error, duckdb_error_data_message(res));
		AppendDuckDBErrorDetails(error, res);
		duckdb_destroy_error_data(&res);
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	return ADBC_STATUS_OK;
}

static AdbcStatusCode IngestToTableFromBoundStream(DuckDBAdbcStatementWrapper *statement, int64_t *rows_affected,
                                                   AdbcError *error) {
	// See ADBC_INGEST_OPTION_TARGET_TABLE
	D_ASSERT(statement->ingestion_stream.release);
	D_ASSERT(statement->ingestion_table_name);

	// Take the input stream from the statement
	auto stream = statement->ingestion_stream;

	// Ingest into a table from the bound stream
	return Ingest(statement->connection, statement->conn_wrapper, statement->target_catalog,
	              statement->ingestion_table_name, statement->db_schema, &stream, error, statement->ingestion_mode,
	              statement->temporary_table, rows_affected);
}

AdbcStatusCode StatementExecuteQuery(struct AdbcStatement *statement, struct ArrowArrayStream *out,
                                     int64_t *rows_affected, struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (!wrapper->connection) {
		SetError(error, "Invalid connection");
		return ADBC_STATUS_INVALID_STATE;
	}

	// Materialize any active streams on this connection before executing a new query.
	// Without materialization, the connection would refuse the new query while a streaming result is open.
	if (wrapper->conn_wrapper) {
		wrapper->conn_wrapper->MaterializeStreams();
		wrapper->conn_wrapper->cancel_requested = false;
	}

	// From here this statement's query is the one running on the connection, which is what its progress options read.
	// Take the connection's progress before publishing the execution number: a reader in between would otherwise see
	// a statement whose execution is not the running one, and report it ended.
	ExecutionProgressGuard progress_guard(wrapper->conn_wrapper, wrapper);
	wrapper->execution = progress_guard.Execution();
	wrapper->completed = false;

	// TODO: Set affected rows, careful with early return
	if (rows_affected) {
		*rows_affected = 0;
	}

	const auto has_stream = wrapper->ingestion_stream.release != nullptr;
	const auto to_table = wrapper->ingestion_table_name != nullptr;

	if (has_stream && to_table) {
		// Ingestion is several queries and an appender rather than one query, so its execution runs until this
		// returns: DuckDB's progress says nothing about it, and it completes only if the ingest succeeded.
		auto ingest_status = IngestToTableFromBoundStream(wrapper, rows_affected, error);
		if (ingest_status == ADBC_STATUS_OK) {
			progress_guard.Completed();
		}
		return ingest_status;
	}

	if (!wrapper->statement) {
		if (out) {
			out->private_data = nullptr;
			out->get_schema = nullptr;
			out->get_next = nullptr;
			out->release = nullptr;
			out->get_last_error = nullptr;
		}

		if (rows_affected) {
			*rows_affected = 0;
		}
		progress_guard.Completed();
		return ADBC_STATUS_OK;
	}

	duckdb::unique_ptr<DuckDBAdbcStreamWrapper> stream_wrapper(new (std::nothrow) DuckDBAdbcStreamWrapper());
	if (!stream_wrapper) {
		SetError(error, "Allocation error");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	stream_wrapper->conn_wrapper = wrapper->conn_wrapper;
	stream_wrapper->execution = progress_guard.Execution();
	stream_wrapper->statement = wrapper;
	auto &prepared = *reinterpret_cast<duckdb::PreparedStatementWrapper *>(wrapper->statement);
	int64_t affected = -1;
	// Only process the stream if there are parameters to bind
	auto prepared_statement_params = prepared.statement->GetParameterCount();
	if (has_stream && prepared_statement_params > 0) {
		// A stream was bound to the statement, use that to bind parameters
		ArrowArrayStream stream = wrapper->ingestion_stream;
		ConvertedSchemaWrapper out_types;
		duckdb::ArrowSchemaWrapper arrow_schema_wrapper;
		stream.get_schema(&stream, &arrow_schema_wrapper.arrow_schema);
		try {
			auto res =
			    duckdb_schema_from_arrow(wrapper->connection, &arrow_schema_wrapper.arrow_schema, out_types.GetPtr());
			if (res) {
				SetError(error, duckdb_error_data_message(res));
				AppendDuckDBErrorDetails(error, res);
				duckdb_destroy_error_data(&res);
				return ADBC_STATUS_INVALID_ARGUMENT;
			}
		} catch (...) {
			return ADBC_STATUS_INTERNAL;
		}

		duckdb::ArrowArrayWrapper arrow_array_wrapper;

		stream.get_next(&stream, &arrow_array_wrapper.arrow_array);

		while (arrow_array_wrapper.arrow_array.release) {
			// This is a valid arrow array, let's make it into a data chunk
			DataChunkWrapper out_chunk;
			auto res_conv = duckdb_data_chunk_from_arrow(wrapper->connection, &arrow_array_wrapper.arrow_array,
			                                             out_types.Get(), &out_chunk.chunk);
			if (res_conv) {
				SetError(error, duckdb_error_data_message(res_conv));
				AppendDuckDBErrorDetails(error, res_conv);
				duckdb_destroy_error_data(&res_conv);
				return ADBC_STATUS_INVALID_ARGUMENT;
			}
			if (!out_chunk.chunk) {
				SetError(error, "Please provide a non-empty chunk to be bound");
				return ADBC_STATUS_INVALID_ARGUMENT;
			}
			auto chunk = reinterpret_cast<duckdb::DataChunk *>(out_chunk.chunk);
			if (chunk->size() == 0) {
				SetError(error, "Please provide a non-empty chunk to be bound");
				return ADBC_STATUS_INVALID_ARGUMENT;
			}
			if (chunk->size() != 1) {
				// TODO: add support for binding multiple rows
				SetError(error, "Binding multiple rows at once is not supported yet");
				return ADBC_STATUS_NOT_IMPLEMENTED;
			}
			if (chunk->ColumnCount() > prepared_statement_params) {
				SetError(error, "Input data has more column than prepared statement has parameters");
				return ADBC_STATUS_INVALID_ARGUMENT;
			}
			duckdb_clear_bindings(wrapper->statement);
			for (idx_t col_idx = 0; col_idx < chunk->ColumnCount(); col_idx++) {
				auto val = chunk->GetValue(col_idx, 0);
				auto duck_val = reinterpret_cast<duckdb_value>(&val);
				auto res = duckdb_bind_value(wrapper->statement, 1 + col_idx, duck_val);
				if (res != DuckDBSuccess) {
					SetError(error, duckdb_prepare_error(wrapper->statement));
					return ADBC_STATUS_INVALID_ARGUMENT;
				}
			}
			// Only the last row's result is handed out, and which row is last is not known without consuming the
			// next one, so no run counts on being read to its end
			auto status = ExecuteArrow(prepared, *stream_wrapper, false, affected, error, wrapper->eager_result);
			if (status != ADBC_STATUS_OK) {
				return status;
			}
			arrow_array_wrapper = duckdb::ArrowArrayWrapper();
			stream.get_next(&stream, &arrow_array_wrapper.arrow_array);
		}
		if (!stream_wrapper->stream && !stream_wrapper->result) {
			SetError(error, "Please provide a non-empty chunk to be bound");
			return ADBC_STATUS_INVALID_ARGUMENT;
		}
	} else {
		auto status = ExecuteArrow(prepared, *stream_wrapper, out != nullptr, affected, error, wrapper->eager_result);
		if (status != ADBC_STATUS_OK) {
			return status;
		}
	}

	// Set rows_affected for queries (if not already set by ingestion path)
	if (rows_affected && !(has_stream && to_table)) {
		*rows_affected = affected;
	}

	if (out) {
		auto *released = stream_wrapper.release();
		out->private_data = released;
		out->get_schema = get_schema;
		out->get_next = get_next;
		out->release = release;
		out->get_last_error = get_last_error;
		if (wrapper->conn_wrapper) {
			wrapper->conn_wrapper->RegisterStream(released);
		}
		// A streaming result's query keeps running as the stream is consumed, and the stream ends the execution when
		// it is exhausted, fails or is released. A result DuckDB could not stream was computed in full before it was
		// returned, so its execution is already complete.
		if (released->stream) {
			progress_guard.HandOver();
		} else {
			progress_guard.Completed();
		}
	} else {
		progress_guard.Completed();
	}

	return ADBC_STATUS_OK;
}

// this is a nop for us
AdbcStatusCode StatementPrepare(struct AdbcStatement *statement, struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementSetSqlQuery(struct AdbcStatement *statement, const char *query, struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!query) {
		SetError(error, "Missing query");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto query_len = strlen(query);
	if (std::all_of(query, query + query_len, duckdb::StringUtil::CharacterIsSpace)) {
		SetError(error, "No statements found");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);

	// Materialize any active streams before preparing
	if (wrapper->conn_wrapper) {
		wrapper->conn_wrapper->MaterializeStreams();
	}

	if (wrapper->ingestion_stream.release) {
		// Release any resources currently held by the ingestion stream before we overwrite it
		wrapper->ingestion_stream.release(&wrapper->ingestion_stream);
		wrapper->ingestion_stream.release = nullptr;
	}
	if (wrapper->statement) {
		duckdb_destroy_prepare(&wrapper->statement);
		wrapper->statement = nullptr;
	}
	duckdb_extracted_statements extracted_statements = nullptr;
	auto extract_statements_size = duckdb_extract_statements(wrapper->connection, query, &extracted_statements);
	auto error_msg_extract_statements = duckdb_extract_statements_error(extracted_statements);
	if (error_msg_extract_statements != nullptr) {
		// Things went wrong when executing internal prepared statement
		SetError(error, error_msg_extract_statements);
		duckdb_destroy_extracted(&extracted_statements);
		return ADBC_STATUS_INTERNAL;
	}

	if (extract_statements_size == 0) {
		// Query is non-empty, but there are no actual statements.
		duckdb_destroy_extracted(&extracted_statements);
		return ADBC_STATUS_OK;
	}

	// Now lets loop over the statements, and execute every one
	for (idx_t i = 0; i < extract_statements_size - 1; i++) {
		duckdb_prepared_statement statement_internal = nullptr;
		auto res =
		    duckdb_prepare_extracted_statement(wrapper->connection, extracted_statements, i, &statement_internal);
		auto error_msg = duckdb_prepare_error(statement_internal);
		auto adbc_status = CheckResult(res, error, error_msg);
		if (adbc_status != ADBC_STATUS_OK) {
			duckdb_destroy_prepare(&statement_internal);
			duckdb_destroy_extracted(&extracted_statements);
			return adbc_status;
		}
		// Execute
		duckdb_arrow out_result;
		res = duckdb_execute_prepared_arrow(statement_internal, &out_result);
		if (res != DuckDBSuccess) {
			SetError(error, duckdb_query_arrow_error(out_result));
			duckdb_destroy_arrow(&out_result);
			duckdb_destroy_prepare(&statement_internal);
			duckdb_destroy_extracted(&extracted_statements);
			return ADBC_STATUS_INVALID_ARGUMENT;
		}
		duckdb_destroy_arrow(&out_result);
		duckdb_destroy_prepare(&statement_internal);
	}

	// Final statement (returned to caller)
	auto res = duckdb_prepare_extracted_statement(wrapper->connection, extracted_statements,
	                                              extract_statements_size - 1, &wrapper->statement);
	auto error_msg = duckdb_prepare_error(wrapper->statement);
	duckdb_destroy_extracted(&extracted_statements);
	return CheckResult(res, error, error_msg);
}

AdbcStatusCode StatementBind(struct AdbcStatement *statement, struct ArrowArray *values, struct ArrowSchema *schemas,
                             struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!values) {
		SetError(error, "Missing values object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!schemas) {
		SetError(error, "Invalid schemas object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (wrapper->ingestion_stream.release) {
		// Free the stream that was previously bound
		wrapper->ingestion_stream.release(&wrapper->ingestion_stream);
	}
	auto status = BatchToArrayStream(values, schemas, &wrapper->ingestion_stream, error);
	return status;
}

AdbcStatusCode StatementBindStream(struct AdbcStatement *statement, struct ArrowArrayStream *values,
                                   struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!values) {
		SetError(error, "Missing values object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (wrapper->ingestion_stream.release) {
		// Release any resources currently held by the ingestion stream before we overwrite it
		wrapper->ingestion_stream.release(&wrapper->ingestion_stream);
	}
	wrapper->ingestion_stream = *values;
	values->release = nullptr;
	return ADBC_STATUS_OK;
}

AdbcStatusCode StatementSetOption(struct AdbcStatement *statement, const char *key, const char *value,
                                  struct AdbcError *error) {
	if (!statement) {
		SetError(error, "Missing statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);

	if (strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
		if (wrapper->ingestion_table_name) {
			free(wrapper->ingestion_table_name);
		}
		wrapper->ingestion_table_name = strdup(value);
		return ADBC_STATUS_OK;
	}
	if (strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
		if (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
			// Align with arrow-adbc PostgreSQL driver behavior: if a schema was set
			// before enabling temporary ingestion, clear it so temporary can proceed.
			// (Some clients set schema by default.)
			if (wrapper->db_schema) {
				free(wrapper->db_schema);
				wrapper->db_schema = nullptr;
			}
			// Some clients may also set a catalog by default; clear it so temporary can proceed.
			if (wrapper->target_catalog) {
				free(wrapper->target_catalog);
				wrapper->target_catalog = nullptr;
			}
			wrapper->temporary_table = true;
			return ADBC_STATUS_OK;
		} else if (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
			wrapper->temporary_table = false;
			return ADBC_STATUS_OK;
		} else {
			SetError(
			    error,
			    "ADBC_INGEST_OPTION_TEMPORARY, can only be ADBC_OPTION_VALUE_ENABLED or ADBC_OPTION_VALUE_DISABLED");
			return ADBC_STATUS_INVALID_ARGUMENT;
		}
	}

	if (strcmp(key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
		if (wrapper->db_schema) {
			free(wrapper->db_schema);
		}
		wrapper->db_schema = strdup(value);
		return ADBC_STATUS_OK;
	}

	if (strcmp(key, ADBC_INGEST_OPTION_TARGET_CATALOG) == 0) {
		if (wrapper->target_catalog) {
			free(wrapper->target_catalog);
		}
		wrapper->target_catalog = strdup(value);
		return ADBC_STATUS_OK;
	}

	if (strcmp(key, ADBC_INGEST_OPTION_MODE) == 0) {
		if (strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE) == 0) {
			wrapper->ingestion_mode = IngestionMode::CREATE;
			return ADBC_STATUS_OK;
		} else if (strcmp(value, ADBC_INGEST_OPTION_MODE_APPEND) == 0) {
			wrapper->ingestion_mode = IngestionMode::APPEND;
			return ADBC_STATUS_OK;
		} else if (strcmp(value, ADBC_INGEST_OPTION_MODE_REPLACE) == 0) {
			wrapper->ingestion_mode = IngestionMode::REPLACE;
			return ADBC_STATUS_OK;
		} else if (strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) == 0) {
			wrapper->ingestion_mode = IngestionMode::CREATE_APPEND;
			return ADBC_STATUS_OK;
		} else {
			SetError(error, "Invalid ingestion mode");
			return ADBC_STATUS_INVALID_ARGUMENT;
		}
	}
	duckdb::stringstream ss;
	ss << "Statement Set Option " << key << " is not yet accepted by DuckDB";
	SetError(error, ss.str());
	return ADBC_STATUS_INVALID_ARGUMENT;
}

// Statement Typed Option API (ADBC 1.1.0)
static const char *IngestionModeToString(IngestionMode mode) {
	switch (mode) {
	case IngestionMode::CREATE:
		return ADBC_INGEST_OPTION_MODE_CREATE;
	case IngestionMode::APPEND:
		return ADBC_INGEST_OPTION_MODE_APPEND;
	case IngestionMode::REPLACE:
		return ADBC_INGEST_OPTION_MODE_REPLACE;
	case IngestionMode::CREATE_APPEND:
		return ADBC_INGEST_OPTION_MODE_CREATE_APPEND;
	default:
		return ADBC_INGEST_OPTION_MODE_CREATE;
	}
}

AdbcStatusCode StatementGetOption(struct AdbcStatement *statement, const char *key, char *value, size_t *length,
                                  struct AdbcError *error) {
	if (!statement || !statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
		if (wrapper->ingestion_table_name) {
			return GetOptionStringHelper(wrapper->ingestion_table_name, value, length, error);
		}
		SetError(error, "Option not set: " ADBC_INGEST_OPTION_TARGET_TABLE);
		return ADBC_STATUS_NOT_FOUND;
	}
	if (strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
		const char *val = wrapper->temporary_table ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
		return GetOptionStringHelper(val, value, length, error);
	}
	if (strcmp(key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
		if (wrapper->db_schema) {
			return GetOptionStringHelper(wrapper->db_schema, value, length, error);
		}
		SetError(error, "Option not set: " ADBC_INGEST_OPTION_TARGET_DB_SCHEMA);
		return ADBC_STATUS_NOT_FOUND;
	}
	if (strcmp(key, ADBC_INGEST_OPTION_TARGET_CATALOG) == 0) {
		if (wrapper->target_catalog) {
			return GetOptionStringHelper(wrapper->target_catalog, value, length, error);
		}
		SetError(error, "Option not set: " ADBC_INGEST_OPTION_TARGET_CATALOG);
		return ADBC_STATUS_NOT_FOUND;
	}
	if (strcmp(key, ADBC_INGEST_OPTION_MODE) == 0) {
		return GetOptionStringHelper(IngestionModeToString(wrapper->ingestion_mode), value, length, error);
	}
	auto error_message = std::string("Option not found: ") + key;
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode StatementGetOptionBytes(struct AdbcStatement *statement, const char *key, uint8_t *value, size_t *length,
                                       struct AdbcError *error) {
	if (!statement || !statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

//! The maximum of ADBC_STATEMENT_OPTION_PROGRESS: progress is DuckDB's estimate of the query's completion, as a
//! fraction. ADBC leaves the scale to the driver, but the Flight SQL driver reports a fraction of 1.0 and Flight's own
//! PollInfo.progress is defined over [0.0, 1.0], so a client that reads progress without dividing by the maximum still
//! gets a sensible number.
static constexpr double STATEMENT_MAX_PROGRESS = 1.0;

//! Progress is kept as a fixed-point fraction packed with the execution it was measured for, so that a reading taken
//! just before a new execution started cannot raise the floor of that new execution.
static constexpr uint64_t PROGRESS_BITS = 20;
static constexpr uint64_t PROGRESS_SCALE = 1000000;

static uint64_t PackProgress(uint64_t execution, double fraction) {
	auto fixed = static_cast<uint64_t>(fraction * static_cast<double>(PROGRESS_SCALE));
	if (fixed > PROGRESS_SCALE) {
		fixed = PROGRESS_SCALE;
	}
	return (execution << PROGRESS_BITS) | fixed;
}

//! The fraction stored for `execution`, or 0 if what is stored belongs to another execution.
static double UnpackProgress(uint64_t packed, uint64_t execution) {
	if ((packed >> PROGRESS_BITS) != execution) {
		return 0;
	}
	return static_cast<double>(packed & ((1ULL << PROGRESS_BITS) - 1)) / static_cast<double>(PROGRESS_SCALE);
}

//! Reads atomics and lock-free query progress while another thread executes or consumes the result.
static double GetStatementProgress(DuckDBAdbcStatementWrapper &wrapper) {
	// The connection's progress belongs to whatever it is running, so a sample of it counts only if this statement
	// still owns that execution both before and after the sample: otherwise the sample was somebody else's.
	for (idx_t attempt = 0; attempt < 4; attempt++) {
		auto execution = wrapper.execution.load();
		if (execution == 0) {
			return 0;
		}
		auto *conn_wrapper = wrapper.connection_released ? nullptr : wrapper.conn_wrapper;
		if (!conn_wrapper || !conn_wrapper->IsRunning(execution)) {
			if (wrapper.completed) {
				return STATEMENT_MAX_PROGRESS;
			}
			// An incomplete execution keeps its last observed progress.
			return UnpackProgress(wrapper.progress_floor.load(), execution);
		}
		double fraction = duckdb_query_progress(wrapper.connection).percentage / 100;
		if (wrapper.execution.load() != execution || !conn_wrapper->IsRunning(execution)) {
			continue;
		}
		if (fraction < 0) {
			// DuckDB reports -1 before the executor starts and again once the query has ended, which for a streamed
			// result is while the client still has rows to read. Report the highest estimate seen for this execution.
			return UnpackProgress(wrapper.progress_floor.load(), execution);
		}
		// Lock-free "store the maximum", tagged with the execution it belongs to.
		auto packed = PackProgress(execution, fraction);
		auto floor = wrapper.progress_floor.load();
		while (true) {
			auto stored = UnpackProgress(floor, execution);
			if (fraction <= stored) {
				return std::min(stored, STATEMENT_MAX_PROGRESS);
			}
			if (wrapper.progress_floor.compare_exchange_weak(floor, packed)) {
				return UnpackProgress(packed, execution);
			}
		}
	}
	// The statement kept being re-executed underneath the reader; it has no reading to report.
	return 0;
}

AdbcStatusCode StatementGetOptionDouble(struct AdbcStatement *statement, const char *key, double *value,
                                        struct AdbcError *error) {
	if (!statement || !statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (key &&
	    (strcmp(key, ADBC_STATEMENT_OPTION_PROGRESS) == 0 || strcmp(key, ADBC_STATEMENT_OPTION_MAX_PROGRESS) == 0)) {
		if (!value) {
			SetError(error, "Missing value");
			return ADBC_STATUS_INVALID_ARGUMENT;
		}
		auto &wrapper = *static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
		// MAX_PROGRESS is the fixed scale, regardless of whether the execution completed.
		*value = strcmp(key, ADBC_STATEMENT_OPTION_MAX_PROGRESS) == 0 ? STATEMENT_MAX_PROGRESS
		                                                              : GetStatementProgress(wrapper);
		return ADBC_STATUS_OK;
	}
	auto error_message = std::string("Option not found: ") + (key ? key : "(null)");
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode StatementGetOptionInt(struct AdbcStatement *statement, const char *key, int64_t *value,
                                     struct AdbcError *error) {
	if (!statement || !statement->private_data) {
		SetError(error, "Invalid statement object");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto wrapper = static_cast<DuckDBAdbcStatementWrapper *>(statement->private_data);
	if (strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
		*value = wrapper->temporary_table ? 1 : 0;
		return ADBC_STATUS_OK;
	}
	auto error_message = std::string("Option not found: ") + key;
	SetError(error, error_message);
	return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode StatementSetOptionBytes(struct AdbcStatement *statement, const char *key, const uint8_t *value,
                                       size_t length, struct AdbcError *error) {
	SetError(error, "SetOptionBytes is not supported for statement");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode StatementSetOptionInt(struct AdbcStatement *statement, const char *key, int64_t value,
                                     struct AdbcError *error) {
	if (!key) {
		SetError(error, "Missing key");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
		const char *str_value = value ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
		return StatementSetOption(statement, key, str_value, error);
	}
	return StatementSetOption(statement, key, std::to_string(value).c_str(), error);
}

AdbcStatusCode StatementSetOptionDouble(struct AdbcStatement *statement, const char *key, double value,
                                        struct AdbcError *error) {
	SetError(error, "SetOptionDouble is not supported for statement");
	return ADBC_STATUS_NOT_IMPLEMENTED;
}

std::string createFilter(const char *input) {
	if (input) {
		return duckdb::SQLString::ToString(input);
	}
	return "'%'";
}

AdbcStatusCode ConnectionGetObjects(struct AdbcConnection *connection, int depth, const char *catalog,
                                    const char *db_schema, const char *table_name, const char **table_type,
                                    const char *column_name, struct ArrowArrayStream *out, struct AdbcError *error) {
	std::string catalog_filter = createFilter(catalog);
	std::string db_schema_filter = createFilter(db_schema);
	std::string table_name_filter = createFilter(table_name);
	std::string column_name_filter = createFilter(column_name);
	std::string table_type_condition = "";
	if (table_type && table_type[0]) {
		table_type_condition = " AND table_type IN (";
		for (int i = 0; table_type[i]; ++i) {
			if ((strcmp(table_type[i], "LOCAL TABLE") != 0) && (strcmp(table_type[i], "BASE TABLE") != 0) &&
			    (strcmp(table_type[i], "VIEW") != 0)) {
				duckdb::stringstream ss;
				ss << "Table type must be \"LOCAL TABLE\", \"BASE TABLE\" or "
				   << "\"VIEW\": \"" << table_type[i] << "\"";
				SetError(error, ss.str());
				return ADBC_STATUS_INVALID_ARGUMENT;
			}
			if (i > 0) {
				table_type_condition += ", ";
			}
			table_type_condition += createFilter(table_type[i]);
		}
		table_type_condition += ")";
	}

	std::string query;
	switch (depth) {
	case ADBC_OBJECT_DEPTH_CATALOGS:
		// Return metadata on catalogs.
		query = duckdb::StringUtil::Format(R"(
				SELECT
					catalog_name,
					[]::STRUCT(
						db_schema_name VARCHAR,
						db_schema_tables STRUCT(
							table_name VARCHAR,
							table_type VARCHAR,
							table_columns STRUCT(
								column_name VARCHAR,
								ordinal_position INTEGER,
								remarks VARCHAR,
								xdbc_data_type SMALLINT,
								xdbc_type_name VARCHAR,
								xdbc_column_size INTEGER,
								xdbc_decimal_digits SMALLINT,
								xdbc_num_prec_radix SMALLINT,
								xdbc_nullable SMALLINT,
								xdbc_column_def VARCHAR,
								xdbc_sql_data_type SMALLINT,
								xdbc_datetime_sub SMALLINT,
								xdbc_char_octet_length INTEGER,
								xdbc_is_nullable VARCHAR,
								xdbc_scope_catalog VARCHAR,
								xdbc_scope_schema VARCHAR,
								xdbc_scope_table VARCHAR,
								xdbc_is_autoincrement BOOLEAN,
								xdbc_is_generatedcolumn BOOLEAN
							)[],
							table_constraints STRUCT(
								constraint_name VARCHAR,
								constraint_type VARCHAR,
								constraint_column_names VARCHAR[],
								constraint_column_usage STRUCT(fk_catalog VARCHAR, fk_db_schema VARCHAR, fk_table VARCHAR, fk_column_name VARCHAR)[]
							)[]
						)[]
					)[] catalog_db_schemas
				FROM
					information_schema.schemata
				WHERE catalog_name LIKE %s
				GROUP BY catalog_name
				)",
		                                   catalog_filter);
		break;
	case ADBC_OBJECT_DEPTH_DB_SCHEMAS:
		// Return metadata on catalogs and schemas.
		query = duckdb::StringUtil::Format(R"(
				WITH db_schemas AS (
					SELECT
						catalog_name,
						schema_name,
					FROM information_schema.schemata
					WHERE schema_name LIKE %s
				)

				SELECT
					catalog_name,
					COALESCE(LIST({
						db_schema_name: schema_name,
						db_schema_tables: []::STRUCT(
							table_name VARCHAR,
							table_type VARCHAR,
							table_columns STRUCT(
								column_name VARCHAR,
								ordinal_position INTEGER,
								remarks VARCHAR,
								xdbc_data_type SMALLINT,
								xdbc_type_name VARCHAR,
								xdbc_column_size INTEGER,
								xdbc_decimal_digits SMALLINT,
								xdbc_num_prec_radix SMALLINT,
								xdbc_nullable SMALLINT,
								xdbc_column_def VARCHAR,
								xdbc_sql_data_type SMALLINT,
								xdbc_datetime_sub SMALLINT,
								xdbc_char_octet_length INTEGER,
								xdbc_is_nullable VARCHAR,
								xdbc_scope_catalog VARCHAR,
								xdbc_scope_schema VARCHAR,
								xdbc_scope_table VARCHAR,
								xdbc_is_autoincrement BOOLEAN,
								xdbc_is_generatedcolumn BOOLEAN
							)[],
							table_constraints STRUCT(
								constraint_name VARCHAR,
								constraint_type VARCHAR,
								constraint_column_names VARCHAR[],
								constraint_column_usage STRUCT(fk_catalog VARCHAR, fk_db_schema VARCHAR, fk_table VARCHAR, fk_column_name VARCHAR)[]
							)[]
						)[],
					}) FILTER (dbs.schema_name is not null), []) catalog_db_schemas
				FROM
					information_schema.schemata
				LEFT JOIN db_schemas dbs
				USING (catalog_name, schema_name)
				WHERE catalog_name LIKE %s
				GROUP BY catalog_name
				)",
		                                   db_schema_filter, catalog_filter);
		break;
	case ADBC_OBJECT_DEPTH_TABLES:
		// Return metadata on catalogs, schemas, and tables.
		query = duckdb::StringUtil::Format(R"(
				WITH tables AS (
					SELECT
						table_catalog catalog_name,
						table_schema schema_name,
						LIST({
							table_name: table_name,
							table_type: table_type,
							table_columns: []::STRUCT(
								column_name VARCHAR,
								ordinal_position INTEGER,
								remarks VARCHAR,
								xdbc_data_type SMALLINT,
								xdbc_type_name VARCHAR,
								xdbc_column_size INTEGER,
								xdbc_decimal_digits SMALLINT,
								xdbc_num_prec_radix SMALLINT,
								xdbc_nullable SMALLINT,
								xdbc_column_def VARCHAR,
								xdbc_sql_data_type SMALLINT,
								xdbc_datetime_sub SMALLINT,
								xdbc_char_octet_length INTEGER,
								xdbc_is_nullable VARCHAR,
								xdbc_scope_catalog VARCHAR,
								xdbc_scope_schema VARCHAR,
								xdbc_scope_table VARCHAR,
								xdbc_is_autoincrement BOOLEAN,
								xdbc_is_generatedcolumn BOOLEAN
							)[],
							table_constraints: []::STRUCT(
								constraint_name VARCHAR,
								constraint_type VARCHAR,
								constraint_column_names VARCHAR[],
								constraint_column_usage STRUCT(fk_catalog VARCHAR, fk_db_schema VARCHAR, fk_table VARCHAR, fk_column_name VARCHAR)[]
							)[],
						}) db_schema_tables
					FROM information_schema.tables
					WHERE table_name LIKE %s%s
					GROUP BY table_catalog, table_schema
				),
				db_schemas AS (
					SELECT
						catalog_name,
						schema_name,
						COALESCE(db_schema_tables, []) AS db_schema_tables,
					FROM information_schema.schemata
					LEFT JOIN tables
					USING (catalog_name, schema_name)
					WHERE schema_name LIKE %s
				)

				SELECT
					catalog_name,
					COALESCE(LIST({
						db_schema_name: schema_name,
						db_schema_tables: db_schema_tables,
					}) FILTER (dbs.schema_name is not null), []) catalog_db_schemas
				FROM
					information_schema.schemata
				LEFT JOIN db_schemas dbs
				USING (catalog_name, schema_name)
				WHERE catalog_name LIKE %s
				GROUP BY catalog_name
				)",
		                                   table_name_filter, table_type_condition, db_schema_filter, catalog_filter);
		break;
	case ADBC_OBJECT_DEPTH_COLUMNS:
		// Return metadata on catalogs, schemas, tables, and columns.
		query = duckdb::StringUtil::Format(R"(
				WITH columns AS (
					SELECT
						table_catalog,
						table_schema,
						table_name,
						LIST({
							column_name: column_name,
							ordinal_position: ordinal_position,
							remarks: comment,
							xdbc_data_type: NULL::SMALLINT, -- Arrow type ID not derivable from SQL; SQL type codes are in xdbc_sql_data_type
							xdbc_type_name: data_type,
							xdbc_column_size: CASE
								WHEN base_type = 'DATE' THEN 10::INTEGER
								WHEN data_type IN ('TIME', 'TIME WITH TIME ZONE', 'TIME_NS') THEN 15::INTEGER
								WHEN data_type LIKE 'TIMESTAMP%%' THEN 26::INTEGER
								ELSE numeric_precision::INTEGER
							END,
							xdbc_decimal_digits: numeric_scale::SMALLINT,
							xdbc_num_prec_radix: numeric_precision_radix::SMALLINT,
							xdbc_nullable: CASE is_nullable
								WHEN FALSE THEN 0::SMALLINT
								WHEN TRUE THEN 1::SMALLINT
								ELSE 2::SMALLINT
							END,
							xdbc_column_def: column_default,
							xdbc_sql_data_type: CASE
								WHEN data_type = 'TIMESTAMP WITH TIME ZONE' THEN 2014::SMALLINT
								WHEN data_type LIKE 'TIMESTAMP%%' THEN 93::SMALLINT
								WHEN data_type = 'TIME WITH TIME ZONE' THEN 2013::SMALLINT
								WHEN data_type LIKE '%%]' THEN 2003::SMALLINT
								WHEN type_codes[base_type] IS NOT NULL THEN type_codes[base_type]::SMALLINT
								ELSE 1111::SMALLINT  -- Types.OTHER: aligned with DuckDB JDBC default for unmapped types
							END,
							xdbc_datetime_sub: CASE
								WHEN base_type = 'DATE' THEN 1::SMALLINT
								WHEN data_type LIKE 'TIMESTAMP%%' THEN 3::SMALLINT
								WHEN data_type IN ('TIME', 'TIME WITH TIME ZONE', 'TIME_NS') THEN 2::SMALLINT
								ELSE NULL::SMALLINT
							END,
							xdbc_char_octet_length: CASE
								WHEN base_type IN ('VARCHAR', 'BLOB') THEN character_maximum_length::INTEGER
								ELSE NULL::INTEGER
							END,
							xdbc_is_nullable: CASE is_nullable
								WHEN FALSE THEN 'NO'
								WHEN TRUE THEN 'YES'
								ELSE ''
							END,
							xdbc_scope_catalog: NULL::VARCHAR,  -- REF types not supported in DuckDB
							xdbc_scope_schema: NULL::VARCHAR,
							xdbc_scope_table: NULL::VARCHAR,
							xdbc_is_autoincrement: NULL::BOOLEAN,    -- not exposed via duckdb_columns()
							xdbc_is_generatedcolumn: NULL::BOOLEAN,  -- not exposed via duckdb_columns()
						}) table_columns
					FROM (
						SELECT
							database_name AS table_catalog,
							schema_name AS table_schema,
							table_name,
							column_name,
							column_index AS ordinal_position,
							comment,
							column_default,
							is_nullable,
							numeric_scale,
							numeric_precision,
							numeric_precision_radix,
							character_maximum_length,
							data_type,
							STRING_SPLIT(data_type, '(')[1] AS base_type, -- normalize typemods for type-code lookup
							-- JDBC java.sql.Types-compatible codes, matching DuckDB JDBC where possible.
							MAP {
								'BOOLEAN': 16,
								'TINYINT': -6,
								'UTINYINT': 5,
								'SMALLINT': 5,
								'USMALLINT': 4,
								'INTEGER': 4,
								'UINTEGER': -5,
								'BIGINT': -5,
								'FLOAT': 6,
								'DOUBLE': 8,
								'DATE': 91,
								'TIME': 92,
								'TIME_NS': 92,
								'VARCHAR': 12,
								'BLOB': 2004,
								'DECIMAL': 3,
								'BIT': -7,
								'STRUCT': 2002,
							} AS type_codes
						FROM duckdb_columns()
						WHERE column_name LIKE %s
					) cols
					GROUP BY table_catalog, table_schema, table_name
				),
				constraints AS (
					SELECT
						database_name AS table_catalog,
						schema_name AS table_schema,
						table_name,
						LIST({
							constraint_name: constraint_name,
							constraint_type: constraint_type,
							constraint_column_names: constraint_column_names,
							constraint_column_usage: list_transform(
								referenced_column_names,
								lambda name: {
									fk_catalog: database_name,
									fk_db_schema: schema_name,
									fk_table: referenced_table,
									fk_column_name: name,
								}
							)
						}) table_constraints
					FROM duckdb_constraints()
					WHERE
						constraint_type NOT IN ('NOT NULL') AND
						list_has_any(
							constraint_column_names,
							list_filter(
								constraint_column_names,
								lambda name: name LIKE %s
							)
						)
					GROUP BY database_name, schema_name, table_name
				),
				tables AS (
					SELECT
						table_catalog catalog_name,
						table_schema schema_name,
						LIST({
							table_name: table_name,
							table_type: table_type,
							table_columns: COALESCE(table_columns, []),
							table_constraints: COALESCE(table_constraints, []),
						}) db_schema_tables
					FROM information_schema.tables
					LEFT JOIN columns
					USING (table_catalog, table_schema, table_name)
					LEFT JOIN constraints
					USING (table_catalog, table_schema, table_name)
					WHERE table_name LIKE %s%s
					GROUP BY table_catalog, table_schema
				),
				db_schemas AS (
					SELECT
						catalog_name,
						schema_name,
						COALESCE(db_schema_tables, []) AS db_schema_tables,
					FROM information_schema.schemata
					LEFT JOIN tables
					USING (catalog_name, schema_name)
					WHERE schema_name LIKE %s
				)

				SELECT
					catalog_name,
					COALESCE(LIST({
						db_schema_name: schema_name,
						db_schema_tables: db_schema_tables,
					}) FILTER (dbs.schema_name is not null), []) catalog_db_schemas
				FROM
					information_schema.schemata
				LEFT JOIN db_schemas dbs
				USING (catalog_name, schema_name)
				WHERE catalog_name LIKE %s
				GROUP BY catalog_name
				)",
		                                   column_name_filter, column_name_filter, table_name_filter,
		                                   table_type_condition, db_schema_filter, catalog_filter);
		break;
	default:
		SetError(error, "Invalid value of Depth");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}

	return QueryInternal(connection, out, query.c_str(), error);
}

AdbcStatusCode ConnectionGetTableTypes(struct AdbcConnection *connection, struct ArrowArrayStream *out,
                                       struct AdbcError *error) {
	const auto q = "SELECT DISTINCT table_type FROM information_schema.tables ORDER BY table_type";
	return QueryInternal(connection, out, q, error);
}

//===--------------------------------------------------------------------===//
// Statistics (ADBC 1.1.0)
//===--------------------------------------------------------------------===//
// The `statistic_value` column of GetStatistics must be a dense union per
// the ADBC spec. DuckDB's Arrow export only produces sparse unions, so the
// result batch cannot be produced with QueryInternal; instead it is built
// manually with malloc'd buffers, using the shared helpers defined above
// (also used by ConnectionGetInfo for the same reason).

// Builds the GetStatistics result schema defined in adbc.h. The caller is
// responsible for releasing `schema` on failure.
static AdbcStatusCode BuildGetStatisticsSchema(struct ArrowSchema &schema, struct AdbcError *error) {
	using duckdb_nanoarrow::ArrowSchemaAllocateChildren;
	using duckdb_nanoarrow::ArrowSchemaInit;
	using duckdb_nanoarrow::ArrowSchemaSetFormat;
	using duckdb_nanoarrow::ArrowSchemaSetName;

	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(&schema, duckdb_nanoarrow::NANOARROW_TYPE_STRUCT));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(&schema, 2));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(schema.children[0], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(schema.children[0], "catalog_name"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(schema.children[1], duckdb_nanoarrow::NANOARROW_TYPE_LIST));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(schema.children[1], "catalog_db_schemas"));
	schema.children[1]->flags &= ~ARROW_FLAG_NULLABLE;
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(schema.children[1], 1));

	auto db_schema_schema = schema.children[1]->children[0];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(db_schema_schema, duckdb_nanoarrow::NANOARROW_TYPE_STRUCT));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(db_schema_schema, "item"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(db_schema_schema, 2));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(db_schema_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(db_schema_schema->children[0], "db_schema_name"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(db_schema_schema->children[1], duckdb_nanoarrow::NANOARROW_TYPE_LIST));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(db_schema_schema->children[1], "db_schema_statistics"));
	db_schema_schema->children[1]->flags &= ~ARROW_FLAG_NULLABLE;
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(db_schema_schema->children[1], 1));

	auto statistics_schema = db_schema_schema->children[1]->children[0];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(statistics_schema, duckdb_nanoarrow::NANOARROW_TYPE_STRUCT));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(statistics_schema, "item"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(statistics_schema, 5));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(statistics_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(statistics_schema->children[0], "table_name"));
	statistics_schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(statistics_schema->children[1], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(statistics_schema->children[1], "column_name"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(statistics_schema->children[2], duckdb_nanoarrow::NANOARROW_TYPE_INT16));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(statistics_schema->children[2], "statistic_key"));
	statistics_schema->children[2]->flags &= ~ARROW_FLAG_NULLABLE;

	// statistic_value: dense union (the bundled nanoarrow has no union type
	// template, so the format string is set directly).
	auto value_schema = statistics_schema->children[3];
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema, duckdb_nanoarrow::NANOARROW_TYPE_UNINITIALIZED));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetFormat(value_schema, "+ud:0,1,2,3"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema, "statistic_value"));
	value_schema->flags &= ~ARROW_FLAG_NULLABLE;
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(value_schema, 4));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[0], duckdb_nanoarrow::NANOARROW_TYPE_INT64));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[0], "int64"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[1], duckdb_nanoarrow::NANOARROW_TYPE_UINT64));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[1], "uint64"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[2], duckdb_nanoarrow::NANOARROW_TYPE_DOUBLE));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[2], "float64"));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(value_schema->children[3], duckdb_nanoarrow::NANOARROW_TYPE_BINARY));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(value_schema->children[3], "binary"));

	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(statistics_schema->children[4], duckdb_nanoarrow::NANOARROW_TYPE_BOOL));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(statistics_schema->children[4], "statistic_is_approximate"));
	statistics_schema->children[4]->flags &= ~ARROW_FLAG_NULLABLE;
	return ADBC_STATUS_OK;
}

// Builds the GetStatisticNames result schema (statistic_name utf8 not null,
// statistic_key int16 not null). The caller releases `schema` on failure.
static AdbcStatusCode BuildGetStatisticNamesSchema(struct ArrowSchema &schema, struct AdbcError *error) {
	using duckdb_nanoarrow::ArrowSchemaAllocateChildren;
	using duckdb_nanoarrow::ArrowSchemaInit;
	using duckdb_nanoarrow::ArrowSchemaSetName;

	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(&schema, duckdb_nanoarrow::NANOARROW_TYPE_STRUCT));
	CHECK_MANUAL_NANOARROW(ArrowSchemaAllocateChildren(&schema, 2));
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(schema.children[0], duckdb_nanoarrow::NANOARROW_TYPE_STRING));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(schema.children[0], "statistic_name"));
	schema.children[0]->flags &= ~ARROW_FLAG_NULLABLE;
	CHECK_MANUAL_NANOARROW(ArrowSchemaInit(schema.children[1], duckdb_nanoarrow::NANOARROW_TYPE_INT16));
	CHECK_MANUAL_NANOARROW(ArrowSchemaSetName(schema.children[1], "statistic_key"));
	schema.children[1]->flags &= ~ARROW_FLAG_NULLABLE;
	return ADBC_STATUS_OK;
}

#undef CHECK_MANUAL_NANOARROW

namespace {
// Row-count statistics grouped as catalog -> schema -> table, mirroring the
// nesting of the GetStatistics result schema.
struct StatisticsTableEntry {
	duckdb::string table_name;
	uint64_t row_count;
};
struct StatisticsSchemaGroup {
	duckdb::string name;
	duckdb::vector<StatisticsTableEntry> tables;
};
struct StatisticsCatalogGroup {
	duckdb::string name;
	duckdb::vector<StatisticsSchemaGroup> schemas;
};
} // namespace

// Assembles the nested GetStatistics batch. The caller releases `array` on
// failure.
static AdbcStatusCode BuildGetStatisticsArray(struct ArrowArray &array,
                                              const duckdb::vector<StatisticsCatalogGroup> &catalogs,
                                              idx_t schema_count, idx_t stat_count, struct AdbcError *error) {
	auto catalog_count = catalogs.size();

	// Root struct: one row per catalog.
	auto status = InitManualArrayNode(&array, 1, 2, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	array.length = static_cast<int64_t>(catalog_count);

	duckdb::vector<duckdb::string> catalog_names;
	duckdb::vector<duckdb::string> schema_names;
	duckdb::vector<duckdb::string> table_names;
	for (auto &catalog : catalogs) {
		catalog_names.push_back(catalog.name);
		for (auto &schema_group : catalog.schemas) {
			schema_names.push_back(schema_group.name);
			for (auto &table : schema_group.tables) {
				table_names.push_back(table.table_name);
			}
		}
	}

	status = BuildManualVarcharArray(array.children[0], catalog_names, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}

	// catalog_db_schemas: list over catalogs.
	auto schemas_list = array.children[1];
	status = InitManualArrayNode(schemas_list, 2, 1, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	schemas_list->length = static_cast<int64_t>(catalog_count);
	auto schema_list_offsets =
	    static_cast<int32_t *>(AllocManualBuffer(schemas_list, 1, sizeof(int32_t) * (catalog_count + 1)));
	if (!schema_list_offsets) {
		SetError(error, "Failed to allocate offsets for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	int32_t schema_offset = 0;
	for (idx_t i = 0; i < catalog_count; i++) {
		schema_list_offsets[i] = schema_offset;
		schema_offset += static_cast<int32_t>(catalogs[i].schemas.size());
	}
	schema_list_offsets[catalog_count] = schema_offset;

	// DB_SCHEMA_SCHEMA struct: one row per (catalog, schema) pair.
	auto schema_struct = schemas_list->children[0];
	status = InitManualArrayNode(schema_struct, 1, 2, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	schema_struct->length = static_cast<int64_t>(schema_count);

	status = BuildManualVarcharArray(schema_struct->children[0], schema_names, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}

	// db_schema_statistics: list over schemas.
	auto stats_list = schema_struct->children[1];
	status = InitManualArrayNode(stats_list, 2, 1, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	stats_list->length = static_cast<int64_t>(schema_count);
	auto stats_list_offsets =
	    static_cast<int32_t *>(AllocManualBuffer(stats_list, 1, sizeof(int32_t) * (schema_count + 1)));
	if (!stats_list_offsets) {
		SetError(error, "Failed to allocate offsets for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	int32_t stat_offset = 0;
	idx_t schema_idx = 0;
	for (auto &catalog : catalogs) {
		for (auto &schema_group : catalog.schemas) {
			stats_list_offsets[schema_idx++] = stat_offset;
			stat_offset += static_cast<int32_t>(schema_group.tables.size());
		}
	}
	stats_list_offsets[schema_count] = stat_offset;

	// STATISTICS_SCHEMA struct: one row per statistic (one ROW_COUNT per table).
	auto stats_struct = stats_list->children[0];
	status = InitManualArrayNode(stats_struct, 1, 5, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	stats_struct->length = static_cast<int64_t>(stat_count);

	status = BuildManualVarcharArray(stats_struct->children[0], table_names, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// column_name: all NULL (row counts are table-level statistics).
	status = BuildManualAllNullVarcharArray(stats_struct->children[1], stat_count, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}

	// statistic_key: always ADBC_STATISTIC_ROW_COUNT_KEY.
	auto key_array = stats_struct->children[2];
	status = InitManualArrayNode(key_array, 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	key_array->length = static_cast<int64_t>(stat_count);
	auto keys = static_cast<int16_t *>(AllocManualBuffer(key_array, 1, sizeof(int16_t) * stat_count));
	if (!keys) {
		SetError(error, "Failed to allocate keys for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	for (idx_t i = 0; i < stat_count; i++) {
		keys[i] = ADBC_STATISTIC_ROW_COUNT_KEY;
	}

	// statistic_value: dense union. The ADBC spec types ROW_COUNT as int64
	// when exact and float64 when approximate; DuckDB row counts are always
	// approximate estimates, so every value is stored in the float64 child
	// (type id 2) and offsets are 0..n-1.
	auto value_array = stats_struct->children[3];
	status = InitManualArrayNode(value_array, 2, 4, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	value_array->length = static_cast<int64_t>(stat_count);
	auto type_ids = static_cast<int8_t *>(AllocManualBuffer(value_array, 0, stat_count));
	auto value_offsets = static_cast<int32_t *>(AllocManualBuffer(value_array, 1, sizeof(int32_t) * stat_count));
	if (!type_ids || !value_offsets) {
		SetError(error, "Failed to allocate union buffers for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	for (idx_t i = 0; i < stat_count; i++) {
		type_ids[i] = 2;
		value_offsets[i] = static_cast<int32_t>(i);
	}
	// int64 / uint64 children: empty.
	status = InitManualArrayNode(value_array->children[0], 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = InitManualArrayNode(value_array->children[1], 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// binary child: empty, but variable-size layouts need an offsets buffer
	// with a single zero entry.
	auto binary_child = value_array->children[3];
	status = InitManualArrayNode(binary_child, 3, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	if (!AllocManualBuffer(binary_child, 1, sizeof(int32_t))) {
		SetError(error, "Failed to allocate union buffers for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	// float64 child: carries all row counts.
	auto float64_child = value_array->children[2];
	status = InitManualArrayNode(float64_child, 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	float64_child->length = static_cast<int64_t>(stat_count);
	auto row_counts = static_cast<double *>(AllocManualBuffer(float64_child, 1, sizeof(double) * stat_count));
	if (!row_counts) {
		SetError(error, "Failed to allocate union buffers for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	idx_t stat_idx = 0;
	for (auto &catalog : catalogs) {
		for (auto &schema_group : catalog.schemas) {
			for (auto &table : schema_group.tables) {
				row_counts[stat_idx++] = static_cast<double>(table.row_count);
			}
		}
	}

	// statistic_is_approximate: all true (row counts come from estimates).
	auto approximate_array = stats_struct->children[4];
	status = InitManualArrayNode(approximate_array, 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	approximate_array->length = static_cast<int64_t>(stat_count);
	auto approximate_bits = static_cast<uint8_t *>(AllocManualBuffer(approximate_array, 1, (stat_count + 7) / 8));
	if (!approximate_bits) {
		SetError(error, "Failed to allocate buffers for statistics result");
		return ADBC_STATUS_INTERNAL;
	}
	memset(approximate_bits, 0xFF, (stat_count + 7) / 8);
	return ADBC_STATUS_OK;
}

AdbcStatusCode ConnectionGetStatistics(struct AdbcConnection *connection, const char *catalog, const char *db_schema,
                                       const char *table_name, char approximate, struct ArrowArrayStream *out,
                                       struct AdbcError *error) {
	if (!connection || !connection->private_data) {
		SetError(error, "Connection is invalid");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!out) {
		SetError(error, "Output parameter was not provided");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	auto conn_wrapper = static_cast<duckdb::DuckDBAdbcConnectionWrapper *>(connection->private_data);
	if (!conn_wrapper->connection) {
		SetError(error, "Connection is not initialized");
		return ADBC_STATUS_INVALID_STATE;
	}
	// The connection refuses a query while a streaming result is open.
	conn_wrapper->MaterializeStreams();
	auto conn = reinterpret_cast<duckdb::Connection *>(conn_wrapper->connection);

	// DuckDB only exposes cheap row-count estimates, so statistics are always
	// returned with statistic_is_approximate = true; the spec allows returning
	// approximate values regardless of the `approximate` argument.
	auto query = duckdb::StringUtil::Format(R"(
		SELECT database_name, schema_name, table_name, estimated_size
		FROM duckdb_tables()
		WHERE NOT internal
			AND database_name LIKE %s
			AND schema_name LIKE %s
			AND table_name LIKE %s
		ORDER BY database_name, schema_name, table_name
	)",
	                                        createFilter(catalog), createFilter(db_schema), createFilter(table_name));
	auto result = conn->Query(query);
	if (result->HasError()) {
		SetError(error, result->GetError());
		return ADBC_STATUS_INTERNAL;
	}

	duckdb::vector<StatisticsCatalogGroup> catalogs;
	idx_t schema_count = 0;
	idx_t stat_count = 0;
	auto rows = result->Collection().GetRows();
	for (idx_t row_idx = 0; row_idx < result->RowCount(); row_idx++) {
		auto size_value = rows.GetValue(3, row_idx);
		if (size_value.IsNull()) {
			continue;
		}
		auto estimated_size = size_value.GetValue<int64_t>();
		if (estimated_size < 0) {
			continue;
		}
		auto catalog_name = rows.GetValue(0, row_idx).GetValue<duckdb::string>();
		auto schema_name = rows.GetValue(1, row_idx).GetValue<duckdb::string>();
		auto current_table = rows.GetValue(2, row_idx).GetValue<duckdb::string>();
		if (catalogs.empty() || catalogs.back().name != catalog_name) {
			catalogs.push_back({catalog_name, {}});
		}
		auto &catalog_group = catalogs.back();
		if (catalog_group.schemas.empty() || catalog_group.schemas.back().name != schema_name) {
			catalog_group.schemas.push_back({schema_name, {}});
			schema_count++;
		}
		catalog_group.schemas.back().tables.push_back({current_table, static_cast<uint64_t>(estimated_size)});
		stat_count++;
	}

	struct ArrowSchema schema;
	memset(&schema, 0, sizeof(schema));
	struct ArrowArray array;
	memset(&array, 0, sizeof(array));
	ManualBatchGuard guard(schema, array);

	auto status = BuildGetStatisticsSchema(schema, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = BuildGetStatisticsArray(array, catalogs, schema_count, stat_count, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// Per ADBC semantics `out` is a pure output parameter that may contain
	// uninitialized stack garbage, so it must be zeroed rather than letting
	// BatchToArrayStream inspect `out->release` (its initialization check is
	// only meaningful for internally managed, zero-initialized streams).
	memset(out, 0, sizeof(*out));
	// On success ownership of schema/array moves into the stream and the guard
	// becomes a no-op.
	return BatchToArrayStream(&array, &schema, out, error);
}

AdbcStatusCode ConnectionGetStatisticNames(struct AdbcConnection *connection, struct ArrowArrayStream *out,
                                           struct AdbcError *error) {
	if (!connection || !connection->private_data) {
		SetError(error, "Connection is invalid");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	if (!out) {
		SetError(error, "Output parameter was not provided");
		return ADBC_STATUS_INVALID_ARGUMENT;
	}
	// DuckDB defines no driver-specific statistics (keys >= 1024), so the
	// result is always empty. It is built manually so the NOT NULL fields
	// match the spec exactly.
	struct ArrowSchema schema;
	memset(&schema, 0, sizeof(schema));
	struct ArrowArray array;
	memset(&array, 0, sizeof(array));
	ManualBatchGuard guard(schema, array);

	auto status = BuildGetStatisticNamesSchema(schema, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = InitManualArrayNode(&array, 1, 2, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = BuildManualVarcharArray(array.children[0], duckdb::vector<duckdb::string>(), error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	status = InitManualArrayNode(array.children[1], 2, 0, error);
	if (status != ADBC_STATUS_OK) {
		return status;
	}
	// `out` may contain uninitialized stack garbage; see ConnectionGetStatistics.
	memset(out, 0, sizeof(*out));
	return BatchToArrayStream(&array, &schema, out, error);
}

int ErrorGetDetailCount(const struct AdbcError *error) {
	if (!error || error->vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA || !error->private_data) {
		return 0;
	}
	const auto *details = static_cast<const DuckDBErrorDetails *>(error->private_data);
	return static_cast<int>(details->entries.size());
}

struct AdbcErrorDetail ErrorGetDetail(const struct AdbcError *error, int index) {
	if (!error || error->vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA || !error->private_data) {
		return {nullptr, nullptr, 0};
	}
	const auto *details = static_cast<const DuckDBErrorDetails *>(error->private_data);
	if (index < 0 || static_cast<size_t>(index) >= details->entries.size()) {
		return {nullptr, nullptr, 0};
	}
	const auto &entry = details->entries[static_cast<size_t>(index)];
	return {entry.first.c_str(), reinterpret_cast<const uint8_t *>(entry.second.c_str()), entry.second.size()};
}

} // namespace duckdb_adbc

void duckdb::DuckDBAdbcConnectionWrapper::RegisterStream(duckdb_adbc::DuckDBAdbcStreamWrapper *stream) {
	const duckdb::lock_guard<duckdb::mutex> guard(stream_mutex);
	active_streams.push_back(stream);
}

void duckdb::DuckDBAdbcConnectionWrapper::UnregisterStream(duckdb_adbc::DuckDBAdbcStreamWrapper *stream) {
	const duckdb::lock_guard<duckdb::mutex> guard(stream_mutex);
	auto it = std::find(active_streams.begin(), active_streams.end(), stream);
	if (it != active_streams.end()) {
		active_streams.erase(it);
	}
}

void duckdb::DuckDBAdbcConnectionWrapper::MaterializeStreams() {
	const duckdb::lock_guard<duckdb::mutex> guard(stream_mutex);
	for (auto *result_wrapper : active_streams) {
		if (!result_wrapper) {
			continue;
		}
		duckdb_adbc::MaterializeStream(*result_wrapper);
	}
}

void duckdb::DuckDBAdbcConnectionWrapper::CloseStreams(const char *reason) {
	const duckdb::lock_guard<duckdb::mutex> guard(stream_mutex);
	for (auto *result_wrapper : active_streams) {
		if (!result_wrapper) {
			continue;
		}
		const duckdb::lock_guard<duckdb::mutex> fetch_guard(result_wrapper->fetch_mutex);
		if (!result_wrapper->stream) {
			continue;
		}
		// A reader may still hold the pointer get_last_error returned, so an error is never replaced
		if (!result_wrapper->last_error) {
			duckdb_adbc::SetStreamError(*result_wrapper, duckdb::ErrorData(duckdb::ExceptionType::TRANSACTION, reason));
		}
		result_wrapper->stream.reset();
		FinishExecution(result_wrapper->statement, result_wrapper->execution, false);
	}
}

void duckdb::DuckDBAdbcConnectionWrapper::RegisterStatement(duckdb_adbc::DuckDBAdbcStatementWrapper *statement) {
	const duckdb::lock_guard<duckdb::mutex> guard(statement_mutex);
	statements.push_back(statement);
}

void duckdb::DuckDBAdbcConnectionWrapper::UnregisterStatement(duckdb_adbc::DuckDBAdbcStatementWrapper *statement) {
	const duckdb::lock_guard<duckdb::mutex> guard(statement_mutex);
	auto it = std::find(statements.begin(), statements.end(), statement);
	if (it != statements.end()) {
		statements.erase(it);
	}
}

void duckdb::DuckDBAdbcConnectionWrapper::DetachAndClearStatements() {
	const duckdb::lock_guard<duckdb::mutex> guard(statement_mutex);
	for (auto *statement : statements) {
		if (statement) {
			statement->connection_released = true;
		}
	}
	statements.clear();
}

void duckdb::DuckDBAdbcConnectionWrapper::FinishExecution(duckdb_adbc::DuckDBAdbcStatementWrapper *statement,
                                                          uint64_t execution, bool completed) {
	uint64_t running = execution;
	if (!running_execution.compare_exchange_strong(running, 0)) {
		// It has already ended, or a later execution owns the connection's progress now: the first ending wins.
		return;
	}
	if (!statement) {
		return;
	}
	const duckdb::lock_guard<duckdb::mutex> guard(statement_mutex);
	if (std::find(statements.begin(), statements.end(), statement) == statements.end()) {
		// The statement was released while its result was still open.
		return;
	}
	if (statement->execution.load() == execution) {
		statement->completed = completed;
	}
}

void duckdb::DuckDBAdbcConnectionWrapper::DetachAndClearStreams() {
	const duckdb::lock_guard<duckdb::mutex> guard(stream_mutex);
	for (auto *stream_wrapper : active_streams) {
		if (stream_wrapper) {
			stream_wrapper->conn_wrapper = nullptr;
		}
	}
	active_streams.clear();
}

static void ReleaseError(struct AdbcError *error) {
	if (error) {
		if (error->message)
			delete[] error->message;
		error->message = nullptr;
		error->release = nullptr;
	}
}

void SetError(struct AdbcError *error, const std::string &message) {
	if (!error)
		return;
	if (error->message) {
		// Append
		std::string buffer = error->message;
		buffer.reserve(buffer.size() + message.size() + 1);
		buffer += '\n';
		buffer += message;
		// Release the old message safely - release may be nullptr if error was already released
		if (error->release) {
			error->release(error);
		} else {
			delete[] error->message;
			error->message = nullptr;
		}

		error->message = new char[buffer.size() + 1];
		buffer.copy(error->message, buffer.size());
		error->message[buffer.size()] = '\0';
	} else {
		error->message = new char[message.size() + 1];
		message.copy(error->message, message.size());
		error->message[message.size()] = '\0';
	}
	error->release = ReleaseError;
}

static void ReleaseStreamErrorDetails(struct AdbcError *error) {
	if (!error) {
		return;
	}
	// message is owned by the stream wrapper (last_error), not freed here
	delete static_cast<DuckDBErrorDetails *>(error->private_data);
	error->private_data = nullptr;
	error->release = nullptr;
}

static void ReleaseErrorWithDuckDBDetails(struct AdbcError *error) {
	if (!error) {
		return;
	}
	delete[] error->message;
	error->message = nullptr;
	delete static_cast<DuckDBErrorDetails *>(error->private_data);
	error->private_data = nullptr;
	error->release = nullptr;
}

static const char *DuckDBErrorTypeToString(duckdb_error_type type) {
	switch (type) {
	case DUCKDB_ERROR_INVALID:
		return "Invalid";
	case DUCKDB_ERROR_OUT_OF_RANGE:
		return "OutOfRange";
	case DUCKDB_ERROR_CONVERSION:
		return "Conversion";
	case DUCKDB_ERROR_UNKNOWN_TYPE:
		return "UnknownType";
	case DUCKDB_ERROR_DECIMAL:
		return "Decimal";
	case DUCKDB_ERROR_MISMATCH_TYPE:
		return "MismatchType";
	case DUCKDB_ERROR_DIVIDE_BY_ZERO:
		return "DivideByZero";
	case DUCKDB_ERROR_OBJECT_SIZE:
		return "ObjectSize";
	case DUCKDB_ERROR_INVALID_TYPE:
		return "InvalidType";
	case DUCKDB_ERROR_SERIALIZATION:
		return "Serialization";
	case DUCKDB_ERROR_TRANSACTION:
		return "Transaction";
	case DUCKDB_ERROR_NOT_IMPLEMENTED:
		return "NotImplemented";
	case DUCKDB_ERROR_EXPRESSION:
		return "Expression";
	case DUCKDB_ERROR_CATALOG:
		return "Catalog";
	case DUCKDB_ERROR_PARSER:
		return "Parser";
	case DUCKDB_ERROR_PLANNER:
		return "Planner";
	case DUCKDB_ERROR_SCHEDULER:
		return "Scheduler";
	case DUCKDB_ERROR_EXECUTOR:
		return "Executor";
	case DUCKDB_ERROR_CONSTRAINT:
		return "Constraint";
	case DUCKDB_ERROR_INDEX:
		return "Index";
	case DUCKDB_ERROR_STAT:
		return "Stat";
	case DUCKDB_ERROR_CONNECTION:
		return "Connection";
	case DUCKDB_ERROR_SYNTAX:
		return "Syntax";
	case DUCKDB_ERROR_SETTINGS:
		return "Settings";
	case DUCKDB_ERROR_BINDER:
		return "Binder";
	case DUCKDB_ERROR_NETWORK:
		return "Network";
	case DUCKDB_ERROR_OPTIMIZER:
		return "Optimizer";
	case DUCKDB_ERROR_NULL_POINTER:
		return "NullPointer";
	case DUCKDB_ERROR_IO:
		return "IO";
	case DUCKDB_ERROR_INTERRUPT:
		return "Interrupt";
	case DUCKDB_ERROR_FATAL:
		return "Fatal";
	case DUCKDB_ERROR_INTERNAL:
		return "Internal";
	case DUCKDB_ERROR_INVALID_INPUT:
		return "InvalidInput";
	case DUCKDB_ERROR_OUT_OF_MEMORY:
		return "OutOfMemory";
	case DUCKDB_ERROR_PERMISSION:
		return "Permission";
	case DUCKDB_ERROR_PARAMETER_NOT_RESOLVED:
		return "ParameterNotResolved";
	case DUCKDB_ERROR_PARAMETER_NOT_ALLOWED:
		return "ParameterNotAllowed";
	case DUCKDB_ERROR_DEPENDENCY:
		return "Dependency";
	case DUCKDB_ERROR_HTTP:
		return "HTTP";
	case DUCKDB_ERROR_MISSING_EXTENSION:
		return "MissingExtension";
	case DUCKDB_ERROR_AUTOLOAD:
		return "Autoload";
	case DUCKDB_ERROR_SEQUENCE:
		return "Sequence";
	case DUCKDB_ERROR_DATA_CORRUPTION:
		return "DataCorruption";
	case DUCKDB_INVALID_CONFIGURATION:
		return "InvalidConfiguration";
	case DUCKDB_ERROR_RESOURCE_IN_USE:
		return "ResourceInUse";
	default:
		return "Unknown";
	}
}

static void AppendDuckDBErrorDetails(struct AdbcError *error, duckdb_error_data res) {
	AppendDuckDBErrorDetails(error, duckdb_error_data_error_type(res));
}

static void AppendDuckDBErrorDetails(struct AdbcError *error, duckdb_error_type type) {
	if (!error || error->vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA) {
		return;
	}
	auto *details = new (std::nothrow) DuckDBErrorDetails();
	if (!details) {
		return;
	}
	details->entries.emplace_back("duckdb:error_type", DuckDBErrorTypeToString(type));
	error->private_data = details;
	error->release = ::ReleaseErrorWithDuckDBDetails;
}
