#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_storage.hpp"

namespace duckdb::capiv2 {

//! The context of a log entry, beyond what the write callback receives directly
class CV2LogSinkWriteInfo {
public:
	explicit CV2LogSinkWriteInfo(const LoggingContext &context) : context(context) {
	}

	const LoggingContext &context;
};

static auto Convert(duckdb_v2_log_sink_write_info_handle info) -> CV2LogSinkWriteInfo * {
	return reinterpret_cast<CV2LogSinkWriteInfo *>(info);
}
static auto Convert(CV2LogSinkWriteInfo *info) -> duckdb_v2_log_sink_write_info_handle {
	return reinterpret_cast<duckdb_v2_log_sink_write_info_handle>(info);
}

//! Carries nothing yet
class CV2LogSinkFlushInfo {};

static auto Convert(CV2LogSinkFlushInfo *info) -> duckdb_v2_log_sink_flush_info_handle {
	return reinterpret_cast<duckdb_v2_log_sink_flush_info_handle>(info);
}

//! A log sink that hands every entry to the caller's write callback. Errors the callbacks report are dropped:
//! logging happens inside arbitrary operations, which a sink must not fail.
class CV2CallbackLogSink final : public LogStorage {
public:
	CV2CallbackLogSink(string name, duckdb_v2_log_sink_write_callback_fn write_cb,
	                   duckdb_v2_log_sink_flush_callback_fn flush_cb, shared_ptr<CV2UserData> user_data)
	    : name(std::move(name)), write_cb(write_cb), flush_cb(flush_cb), user_data(std::move(user_data)) {
	}

	const string GetStorageName() override {
		return name;
	}

	void WriteLogEntry(timestamp_t timestamp, LogLevel level, const string &log_type, const string &log_message,
	                   const RegisteredLoggingContext &context) override {
		CV2LogSinkWriteInfo info(context.context);
		duckdb_v2_str type {log_type.data(), log_type.size()};
		duckdb_v2_str message {log_message.data(), log_message.size()};
		CV2ErrorInfo err = {};
		auto err_ptr = Convert(&err);
		// the values of the level and scope enums match duckdb::LogLevel and duckdb::LogContextScope
		write_cb(Convert(&info), GetUserData(), timestamp.value, static_cast<DUCKDB_V2_LOG_LEVEL>(level),
		         static_cast<DUCKDB_V2_LOG_SCOPE>(context.context.scope), &type, &message, &err_ptr);
	}

	void WriteLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) override {
	}

	void FlushAll() override {
		if (!flush_cb) {
			return;
		}
		CV2LogSinkFlushInfo info;
		CV2ErrorInfo err = {};
		auto err_ptr = Convert(&err);
		flush_cb(Convert(&info), GetUserData(), &err_ptr);
	}

	void Flush(LoggingTargetTable table) override {
	}

	bool IsEnabled(LoggingTargetTable table) override {
		return true;
	}

private:
	void *GetUserData() const {
		return user_data ? user_data->GetData() : nullptr;
	}

	const string name;
	duckdb_v2_log_sink_write_callback_fn write_cb;
	duckdb_v2_log_sink_flush_callback_fn flush_cb;
	shared_ptr<CV2UserData> user_data;
};

class CV2LogSink {
public:
	explicit CV2LogSink(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and registers the sink on `target`.
	void Register(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "log sink");
		if (name.empty()) {
			throw InvalidInputException("Log sink name cannot be empty.");
		}
		if (!write_cb) {
			throw InvalidInputException("A write callback must be set for the log sink.");
		}
		// the log manager looks registered sinks up by their lowercased name
		auto sink_name = StringUtil::Lower(name);
		if (sink_name == LogConfig::IN_MEMORY_STORAGE_NAME || sink_name == LogConfig::STDOUT_STORAGE_NAME ||
		    sink_name == LogConfig::FILE_STORAGE_NAME) {
			throw InvalidInputException("\"%s\" is the name of a log sink DuckDB provides.", name);
		}
		shared_ptr<LogStorage> sink = make_shared_ptr<CV2CallbackLogSink>(sink_name, write_cb, flush_cb, user_data);
		if (!target.GetLogManager().RegisterLogStorage(sink_name, sink)) {
			throw InvalidInputException("A log sink named \"%s\" is already registered.", name);
		}
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	string name;
	duckdb_v2_log_sink_write_callback_fn write_cb = nullptr;
	duckdb_v2_log_sink_flush_callback_fn flush_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

static auto Convert(duckdb_v2_log_sink_handle sink) -> CV2LogSink * {
	return reinterpret_cast<CV2LogSink *>(sink);
}
static auto Convert(CV2LogSink *sink) -> duckdb_v2_log_sink_handle {
	return reinterpret_cast<duckdb_v2_log_sink_handle>(sink);
}

static void ReadId(const optional_idx &source, idx_t *id, bool *present) {
	*present = source.IsValid();
	if (source.IsValid()) {
		*id = source.GetIndex();
	}
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_log_sink_create(duckdb_v2_factory_handle factory, duckdb_v2_log_sink_handle *sink,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(sink);
	*sink = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2LogSink>(Convert(factory)->GetDatabase());
		*sink = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_set_name(duckdb_v2_log_sink_handle sink, const duckdb_v2_identifier_t *name,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(sink);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(sink)->name = duckdb::string(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_set_user_data(duckdb_v2_log_sink_handle sink, duckdb_v2_opaque *data,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(sink);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(sink)->user_data = duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_set_write_callback(duckdb_v2_log_sink_handle sink,
                                                      duckdb_v2_log_sink_write_callback_fn callback,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(sink);
	return WithErrorHandler(err, [&]() { Convert(sink)->write_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_set_flush_callback(duckdb_v2_log_sink_handle sink,
                                                      duckdb_v2_log_sink_flush_callback_fn callback,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(sink);
	return WithErrorHandler(err, [&]() { Convert(sink)->flush_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_write_info_get_connection_id(duckdb_v2_log_sink_write_info_handle info, idx_t *id,
                                                                bool *present, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(id);
	DUCKDB_CHECK_ARG(present);
	return WithErrorHandler(err, [&]() { ReadId(Convert(info)->context.connection_id, id, present); });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_write_info_get_transaction_id(duckdb_v2_log_sink_write_info_handle info, idx_t *id,
                                                                 bool *present, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(id);
	DUCKDB_CHECK_ARG(present);
	return WithErrorHandler(err, [&]() { ReadId(Convert(info)->context.transaction_id, id, present); });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_write_info_get_query_id(duckdb_v2_log_sink_write_info_handle info, idx_t *id,
                                                           bool *present, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(id);
	DUCKDB_CHECK_ARG(present);
	return WithErrorHandler(err, [&]() { ReadId(Convert(info)->context.query_id, id, present); });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_write_info_get_thread_id(duckdb_v2_log_sink_write_info_handle info, idx_t *id,
                                                            bool *present, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(id);
	DUCKDB_CHECK_ARG(present);
	return WithErrorHandler(err, [&]() { ReadId(Convert(info)->context.thread_id, id, present); });
}

DUCKDB_V2_ERROR duckdb_v2_instance_register_log_sink(duckdb_v2_instance_handle instance, duckdb_v2_log_sink_handle sink,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(sink);
	return WithErrorHandler(err, [&]() { Convert(sink)->Register(*Convert(instance)->GetDatabase().instance); });
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_log_sink(duckdb_v2_extension_handle extension,
                                                      duckdb_v2_log_sink_handle sink,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(sink);
	return WithErrorHandler(err,
	                        [&]() { Convert(sink)->Register(GetExtensionLoader(extension).GetDatabaseInstance()); });
}

DUCKDB_V2_ERROR duckdb_v2_log_sink_destroy(duckdb_v2_log_sink_handle *sink) {
	return WithErrorHandler(nullptr, [&]() {
		if (!sink) {
			return;
		}
		if (*sink) {
			delete Convert(*sink);
			*sink = nullptr;
		}
	});
}
