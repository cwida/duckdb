#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"

namespace duckdb {

class CallbackLogSink : public LogSink {
public:
	CallbackLogSink(const string &name, duckdb_logger_write_log_entry_t write_log_entry_fun, void *extra_data,
	                duckdb_delete_callback_t delete_callback)
	    : name(name), write_log_entry_fun(write_log_entry_fun), extra_data(extra_data),
	      delete_callback(delete_callback) {
	}

	~CallbackLogSink() override {
		if (!extra_data || !delete_callback) {
			return;
		}
		delete_callback(extra_data);
	}

	void WriteLogEntry(timestamp_t timestamp, LogLevel level, const string &log_type, const string &log_message,
	                   const RegisteredLoggingContext &context) override {
		if (write_log_entry_fun == nullptr) {
			return;
		}
		auto c_timestamp = reinterpret_cast<duckdb_timestamp *>(&timestamp);
		write_log_entry_fun(extra_data, c_timestamp, EnumUtil::ToChars(level), log_type.c_str(), log_message.c_str());
	};

	void WriteLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) override {};

	void Flush(LoggingTargetTable table) override {};

	void FlushAll() override {};

	bool IsEnabled(LoggingTargetTable table) override {
		return true;
	}

	const string GetSinkName() override {
		return name;
	}

private:
	const string name;
	duckdb_logger_write_log_entry_t write_log_entry_fun;
	void *extra_data;
	duckdb_delete_callback_t delete_callback;
};

struct LogStorageWrapper {
	string name;
	duckdb_logger_write_log_entry_t write_log_entry = nullptr;
	void *extra_data = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

} // namespace duckdb

using duckdb::DatabaseWrapper;
using duckdb::LogStorageWrapper;

duckdb_log_storage duckdb_create_log_storage() {
	auto log_storage_wrapper = new LogStorageWrapper();
	return reinterpret_cast<duckdb_log_storage>(log_storage_wrapper);
}

void duckdb_destroy_log_storage(duckdb_log_storage *log_storage) {
	if (log_storage && *log_storage) {
		auto log_storage_wrapper = reinterpret_cast<LogStorageWrapper *>(*log_storage);
		if (log_storage_wrapper->extra_data && log_storage_wrapper->delete_callback) {
			log_storage_wrapper->delete_callback(log_storage_wrapper->extra_data);
		}
		delete log_storage_wrapper;
		*log_storage = nullptr;
	}
}

void duckdb_log_storage_set_write_log_entry(duckdb_log_storage log_storage, duckdb_logger_write_log_entry_t function) {
	if (!log_storage || !function) {
		return;
	}

	auto log_storage_wrapper = reinterpret_cast<LogStorageWrapper *>(log_storage);
	log_storage_wrapper->write_log_entry = function;
}

void duckdb_log_storage_set_extra_data(duckdb_log_storage log_storage, void *extra_data,
                                       duckdb_delete_callback_t delete_callback) {
	if (!log_storage) {
		return;
	}

	auto log_storage_wrapper = reinterpret_cast<LogStorageWrapper *>(log_storage);
	log_storage_wrapper->extra_data = extra_data;
	log_storage_wrapper->delete_callback = delete_callback;
}

void duckdb_log_storage_set_name(duckdb_log_storage log_storage, const char *name) {
	if (!log_storage || !name) {
		return;
	}
	auto log_storage_wrapper = reinterpret_cast<LogStorageWrapper *>(log_storage);
	log_storage_wrapper->name = name;
}

duckdb_state duckdb_register_log_storage(duckdb_database database, duckdb_log_storage log_storage) {
	if (!database || !log_storage) {
		return DuckDBError;
	}

	const auto db_wrapper = reinterpret_cast<DatabaseWrapper *>(database);
	auto log_storage_wrapper = reinterpret_cast<LogStorageWrapper *>(log_storage);
	if (log_storage_wrapper->name.empty() || log_storage_wrapper->write_log_entry == nullptr) {
		return DuckDBError;
	}

	const auto &db = *db_wrapper->database;
	auto shared_storage_ptr = duckdb::make_shared_ptr<duckdb::CallbackLogSink>(
	    log_storage_wrapper->name, log_storage_wrapper->write_log_entry, log_storage_wrapper->extra_data,
	    log_storage_wrapper->delete_callback);
	duckdb::shared_ptr<duckdb::LogSink> storage_ptr = shared_storage_ptr;

	const auto success = db.instance->GetLogManager().RegisterLogSink(log_storage_wrapper->name, storage_ptr);
	if (!success) {
		return DuckDBError;
	}

	// Avoid leaking memory in case of registration failure because
	// we transfer ownership when creating the shared pointer.
	log_storage_wrapper->extra_data = nullptr;
	log_storage_wrapper->delete_callback = nullptr;
	return DuckDBSuccess;
}
