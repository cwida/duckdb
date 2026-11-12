#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/logging/log_manager.hpp"

using namespace duckdb;

template <class SOURCE, typename FUN>
void LogSimpleCustomType(SOURCE &src, FUN f) {
	f("custom_type", src, "log-a-lot: 'simple with type'");
}

template <class SOURCE, typename FUN>
void LogFormatStringCustomType(SOURCE &src, FUN f) {
	f("custom_type", src, "log-a-lot: '%s'", "format with type");
}

#define TEST_ALL_LOG_MACROS(LOG_LEVEL, SOURCE, LOG_TYPE)                                                               \
	DUCKDB_LOG_INTERNAL(SOURCE, "default", LOG_LEVEL, "log-a-lot: 'simple'")                                           \
	DUCKDB_LOG_INTERNAL(SOURCE, "default", LOG_LEVEL, "log-a-lot: '%s'", "format")                                     \
	DUCKDB_LOG_INTERNAL(SOURCE, "default", LOG_LEVEL, string("log-a-lot: 'string type'"))                              \
	DUCKDB_LOG_INTERNAL(SOURCE, "custom_type", LOG_LEVEL, "log-a-lot: 'simple with type'")                             \
	DUCKDB_LOG_INTERNAL(SOURCE, "custom_type", LOG_LEVEL, "log-a-lot: '%s'", "format with type")                       \
	DUCKDB_LOG_INTERNAL(SOURCE, "custom_type", LOG_LEVEL, string("log-a-lot: 'string type with type'"))

// Tests all Logger function entrypoints at the specified log level with the specified enabled/disabled loggers
void test_logging(const string &minimum_level, const string &enabled_log_types, const string &disabled_log_types) {
	DuckDB db(nullptr);
	Connection con(db);

	duckdb::vector<Value> default_types = {"default", "default"};
	duckdb::vector<string> log_levels = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
	auto minimum_level_index = std::find(log_levels.begin(), log_levels.end(), minimum_level) - log_levels.begin();

	REQUIRE_NO_FAIL(con.Query("set enable_logging=true;"));
	REQUIRE_NO_FAIL(con.Query("set logging_level='" + minimum_level + "';"));
	if (!enabled_log_types.empty()) {
		REQUIRE_NO_FAIL(con.Query("set enabled_log_types='" + enabled_log_types + "';"));
		REQUIRE_NO_FAIL(con.Query("set logging_mode='enable_selected';"));
	}
	if (!disabled_log_types.empty()) {
		REQUIRE_NO_FAIL(con.Query("set disabled_log_types='" + disabled_log_types + "';"));
		REQUIRE_NO_FAIL(con.Query("set logging_mode='disable_selected';"));
	}

	bool level_only = (enabled_log_types.empty() && disabled_log_types.empty());
	bool enabled_mode = !enabled_log_types.empty();
	bool disabled_mode = !disabled_log_types.empty();

	// Log all to global logger
	TEST_ALL_LOG_MACROS(LogLevel::LOG_TRACE, *db.instance, DatabaseInstance);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_DEBUG, *db.instance, DatabaseInstance);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_INFO, *db.instance, DatabaseInstance);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_WARNING, *db.instance, DatabaseInstance);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_ERROR, *db.instance, DatabaseInstance);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_FATAL, *db.instance, DatabaseInstance);

	// Log all to client context logger
	TEST_ALL_LOG_MACROS(LogLevel::LOG_TRACE, *con.context, ClientContext);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_DEBUG, *con.context, ClientContext);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_INFO, *con.context, ClientContext);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_WARNING, *con.context, ClientContext);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_ERROR, *con.context, ClientContext);
	TEST_ALL_LOG_MACROS(LogLevel::LOG_FATAL, *con.context, ClientContext);

	// Generate expected log messages
	duckdb::vector<Value> expected_types;
	duckdb::vector<Value> expected_messages;
	duckdb::vector<Value> expected_log_levels;
	idx_t num_runs = 2;
	idx_t num_log_levels = 6;

	for (idx_t i = 0; i < num_runs; i++) {
		for (idx_t j = minimum_level_index; j < num_log_levels; j++) {
			if (level_only ||
			    (enabled_mode && enabled_log_types.find(default_types[i].ToString()) != enabled_log_types.npos) ||
			    (disabled_mode && disabled_log_types.find(default_types[i].ToString()) == enabled_log_types.npos)) {
				expected_messages.push_back("log-a-lot: 'simple'");
				expected_messages.push_back("log-a-lot: 'format'");
				expected_messages.push_back("log-a-lot: 'string type'");
				expected_types.push_back(default_types[i]);
				expected_types.push_back(default_types[i]);
				expected_types.push_back(default_types[i]);
				expected_log_levels.push_back(Value(log_levels[j]));
				expected_log_levels.push_back(Value(log_levels[j]));
				expected_log_levels.push_back(Value(log_levels[j]));
			}

			if (level_only || (enabled_mode && enabled_log_types.find("custom_type") != enabled_log_types.npos) ||
			    (disabled_mode && disabled_log_types.find("custom_type") == enabled_log_types.npos)) {
				expected_messages.push_back("log-a-lot: 'simple with type'");
				expected_messages.push_back("log-a-lot: 'format with type'");
				expected_messages.push_back("log-a-lot: 'string type with type'");
				expected_types.push_back("custom_type");
				expected_types.push_back("custom_type");
				expected_types.push_back("custom_type");
				expected_log_levels.push_back(Value(log_levels[j]));
				expected_log_levels.push_back(Value(log_levels[j]));
				expected_log_levels.push_back(Value(log_levels[j]));
			}
		}
	}

	auto res = con.Query("SELECT type, log_level, message, from duckdb_logs where starts_with(message, 'log-a-lot:')");
	REQUIRE(CHECK_COLUMN(res, 0, expected_types));
	REQUIRE(CHECK_COLUMN(res, 1, expected_log_levels));
	REQUIRE(CHECK_COLUMN(res, 2, expected_messages));
}

// This tests
// - all log levels
// - all combinations of log levels and having either enabled_log_types or disabled_log_types
TEST_CASE("Test logging", "[logging][.]") {
	duckdb::vector<string> log_levels = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
	for (const auto &level : log_levels) {
		// Test in regular mode without explicitly enabled or disabled loggers
		test_logging(level, "", "");

		// Test various combinations of enabled and disabled loggers
		test_logging(level, "custom_type,default", "");
		test_logging(level, "custom_type", "");
		test_logging(level, "", "default");
		test_logging(level, "", "custom_type,default");
	}
}

struct TestLoggingData : public LocalTableFunctionState {
	explicit TestLoggingData(ExecutionContext &context_p) : context(context_p) {};
	ExecutionContext &context;
};

static duckdb::unique_ptr<LocalTableFunctionState>
TestLoggingInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
	return make_uniq<TestLoggingData>(context);
}

static duckdb::unique_ptr<FunctionData> TestLoggingBind(ClientContext &context, TableFunctionBindInput &input,
                                                        duckdb::vector<LogicalType> &return_types,
                                                        duckdb::vector<Identifier> &names) {
	names.emplace_back("value");
	return_types.emplace_back(LogicalType::INTEGER);

	return make_uniq<TableFunctionData>();
}

static void TestLoggingFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &local_state = data_p.local_state->Cast<TestLoggingData>();
	DUCKDB_LOG_INTERNAL(local_state.context, "duckdb.", LogLevel::LOG_INFO, "thread_logger");
}

// This test the thread context logger
TEST_CASE("Test thread context logger", "[logging][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	duckdb::TableFunction tf("test_thread_logger", {}, TestLoggingFunction, TestLoggingBind, nullptr,
	                         TestLoggingInitLocal);
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "log_test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(tf);

	REQUIRE_NO_FAIL(con.Query("set enable_logging=true;"));

	// Run our dummy table function to call the thread local logger
	REQUIRE_NO_FAIL(con.Query("FROM test_thread_logger()"));

	auto res = con.Query("SELECT scope, message, from duckdb_logs where starts_with(message, 'thread_logger')");
	REQUIRE(CHECK_COLUMN(res, 0, {"THREAD"}));
	REQUIRE(CHECK_COLUMN(res, 1, {"thread_logger"}));
}

// Testing pluggable log sink
class MyLogSink : public LogSink {
public:
	void WriteLogEntry(timestamp_t timestamp, LogLevel level, const string &log_type, const string &log_message,
	                   const RegisteredLoggingContext &context) override {
		log_store.insert(log_message);
	};
	void WriteLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) override {};
	void Flush(LoggingTargetTable table) override {};
	void FlushAll() override {};
	bool IsEnabled(LoggingTargetTable table) override {
		return table == LoggingTargetTable::ALL_LOGS;
	}
	const string GetSinkName() override {
		return "MyLogSink";
	}

	unordered_set<string> log_store;
};

TEST_CASE("Test pluggable log sink", "[logging][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto my_log_sink = make_shared_ptr<MyLogSink>();

	duckdb::shared_ptr<LogSink> base_ptr = my_log_sink;
	db.instance->GetLogManager().RegisterLogSink("my_log_sink", base_ptr);

	REQUIRE_NO_FAIL(con.Query("set enable_logging=true;"));
	REQUIRE_NO_FAIL(con.Query("set logging_sink='my_log_sink';"));

	REQUIRE_NO_FAIL(con.Query("select write_log('HELLO, BRO');"));

	REQUIRE(my_log_sink->log_store.find("HELLO, BRO") != my_log_sink->log_store.end());
}

static bool ContainsMessage(const unordered_set<string> &messages, const string &needle) {
	for (auto &message : messages) {
		if (StringUtil::Contains(message, needle)) {
			return true;
		}
	}
	return false;
}

TEST_CASE("Test deprecated logging_storage setting and shell_log_storage sink name", "[logging]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &log_manager = db.instance->GetLogManager();

	// The first sink records the deprecation warnings, the second is registered under the name the shell uses
	auto warning_sink = make_shared_ptr<MyLogSink>();
	auto shell_sink = make_shared_ptr<MyLogSink>();
	duckdb::shared_ptr<LogSink> warning_ptr = warning_sink;
	duckdb::shared_ptr<LogSink> shell_ptr = shell_sink;
	REQUIRE(log_manager.RegisterLogSink("warning_sink", warning_ptr));
	REQUIRE(log_manager.RegisterLogSink("shell_log_sink", shell_ptr));

	REQUIRE_NO_FAIL(con.Query("set enable_logging=true;"));
	REQUIRE_NO_FAIL(con.Query("set logging_sink='warning_sink';"));
	REQUIRE(!ContainsMessage(warning_sink->log_store, "is deprecated"));

	// the deprecated setting reads and writes the same sink, and warns
	auto result = con.Query("select current_setting('logging_storage')");
	REQUIRE(CHECK_COLUMN(result, 0, {"warning_sink"}));
	REQUIRE_NO_FAIL(con.Query("set logging_storage='warning_sink';"));
	REQUIRE(ContainsMessage(warning_sink->log_store, "the logging_storage setting is deprecated: use logging_sink"));

	// the old name of the shell sink resolves to the new one, and warns
	REQUIRE(!ContainsMessage(warning_sink->log_store, "has been renamed"));
	REQUIRE_NO_FAIL(con.Query("set logging_sink='shell_log_storage';"));
	REQUIRE(ContainsMessage(warning_sink->log_store, "'shell_log_storage' has been renamed to 'shell_log_sink'"));
	REQUIRE(log_manager.GetConfig().storage == "shell_log_sink");
	REQUIRE_NO_FAIL(con.Query("select write_log('HELLO, RENAMED SINK');"));
	REQUIRE(shell_sink->log_store.find("HELLO, RENAMED SINK") != shell_sink->log_store.end());

	// same through enable_logging
	REQUIRE_NO_FAIL(con.Query("set logging_sink='warning_sink';"));
	warning_sink->log_store.clear();
	REQUIRE_NO_FAIL(con.Query("call enable_logging(storage = 'shell_log_storage');"));
	REQUIRE(ContainsMessage(warning_sink->log_store, "'shell_log_storage' has been renamed to 'shell_log_sink'"));
	REQUIRE(log_manager.GetConfig().storage == "shell_log_sink");
}

struct CorrectLogType : public LogType {
	static constexpr const char *NAME = "CorrectLogType";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	// Correctly using the unstructured type constructor for VARCHAR
	CorrectLogType() : LogType(NAME, LEVEL) {
	}

	template <typename... ARGS>
	static string ConstructLogMessage(const string &str, ARGS... params) {
		return StringUtil::Format(str, params...);
	}
};

constexpr LogLevel CorrectLogType::LEVEL;

TEST_CASE("Add LogType with VARCHAR type", "[logging][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	LogManager &log_manager = db.instance->GetLogManager();

	REQUIRE_NOTHROW(log_manager.RegisterLogType(make_uniq<CorrectLogType>()));
}
