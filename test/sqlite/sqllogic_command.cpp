#include "sqllogic_command.hpp"
#include "duckdb/main/database_manager.hpp"
#include "sqllogic_test_runner.hpp"
#include "result_helper.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "test_helpers.hpp"
#include "test_config.hpp"
#include "sqllogic_test_logger.hpp"
#include "test_reporter.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <random>
#include <thread>

namespace duckdb {

static void query_break(int line) {
	(void)line;
}

//! Thrown when the query watchdog interrupted a query in a parallel worker, carrying the failing statement's
//! location so the worker's failure bookkeeping can report it structurally.
class QueryTimeoutException : public std::runtime_error {
public:
	QueryTimeoutException(string message, string file_p, int line_p)
	    : std::runtime_error(std::move(message)), file(std::move(file_p)), line(line_p) {
	}

	string file;
	int line;
};

//! Interrupts the connection if the query does not finish within the configured timeout. This also bounds
//! queries that deadlock idle: the executor's wait loop checks the interrupt flag, but not the query deadline,
//! so SET max_execution_time cannot abort a fully parked schedule.
class QueryWatchdog {
public:
	QueryWatchdog(Connection &connection_p, double timeout_seconds) : connection(connection_p), done(false) {
		if (timeout_seconds <= 0) {
			return;
		}
		watchdog = std::thread([this, timeout_seconds]() {
			std::unique_lock<std::mutex> guard(lock);
			if (cv.wait_for(guard, std::chrono::duration<double>(timeout_seconds), [this]() { return done; })) {
				return;
			}
			// Keep interrupting until the query ends: an interrupt delivered before query startup is erased by
			// the query's own initial cleanup, so a single shot could be lost.
			fired = true;
			do {
				connection.Interrupt();
			} while (!cv.wait_for(guard, std::chrono::milliseconds(100), [this]() { return done; }));
		});
	}
	//! Stops the watchdog and reports whether it interrupted the query.
	bool Finish() {
		if (!watchdog.joinable()) {
			return false;
		}
		{
			std::lock_guard<std::mutex> guard(lock);
			done = true;
		}
		cv.notify_all();
		watchdog.join();
		if (fired && connection.context->IsInterrupted()) {
			// The interrupt may have raced with a successful query completion. Clear the flag so it cannot
			// spuriously abort the next statement. The IsInterrupted check keeps an INTERRUPTS_SUPPRESSED
			// state intact, and if the query was aborted the engine already cleared the flag.
			connection.context->ClearInterrupt();
		}
		return fired;
	}

	~QueryWatchdog() {
		Finish();
	}

private:
	Connection &connection;
	std::mutex lock;
	std::condition_variable cv;
	bool done;
	bool fired = false;
	std::thread watchdog;
};

static Connection &GetConnection(SQLLogicTestRunner &runner, DuckDB &db,
                                 unordered_map<string, duckdb::unique_ptr<Connection>> &named_connection_map,
                                 const string &con_name) {
	auto entry = named_connection_map.find(con_name);
	if (entry == named_connection_map.end()) {
		// not found: create a new connection
		auto con = make_uniq<Connection>(db);

		auto &test_config = TestConfiguration::Get();
		auto init_cmd = test_config.OnConnectionCommand();
		if (!init_cmd.empty()) {
			auto res = con->Query(runner.ReplaceKeywords(init_cmd));
			if (res->HasError()) {
				TEST_FAIL("Startup queries provided via on_new_connection failed: " + res->GetError());
			}
		}
		auto &res = *con;

		named_connection_map[con_name] = std::move(con);
		return res;
	}
	if (!RefersToSameObject(*entry->second->context->db, *db.instance)) {
		TEST_FAIL("Named connection has been started with different target databases");
	}
	return *entry->second;
}
static Connection &GetConnection(SQLLogicTestRunner &runner,
                                 unordered_map<string, duckdb::unique_ptr<Connection>> &named_connection_map,
                                 const string &con_name) {
	if (StringUtil::Contains(con_name, ":")) {
		auto splits = StringUtil::Split(con_name, ":");
		if (splits.size() != 2) {
			TEST_FAIL("Expected either connection name or database:connection");
		}
		auto &db_name = splits[0];
		auto &con_name = splits[1];
		auto db_map = runner.named_db.find(db_name);
		if (db_map != runner.named_db.end()) {
			// we already have a database - connect to it
			return GetConnection(runner, *db_map->second, named_connection_map, con_name);
		}
		// no database - create it
		if (runner.named_connection_map.find(con_name) != runner.named_connection_map.end()) {
			TEST_FAIL("Database did not exist, but named connection already existed");
		}
		auto result = runner.CreateDatabase(":memory:", true);
		auto &result_con = *result.con;
		runner.named_db[db_name] = std::move(result.db);
		runner.named_connection_map[con_name] = std::move(result.con);
		return result_con;
	} else {
		return GetConnection(runner, *runner.db, named_connection_map, con_name);
	}
}

Command::Command(SQLLogicTestRunner &runner) : runner(runner), query_timeout(runner.query_timeout) {
}

Command::~Command() {
}

Connection &Command::CommandConnection(ExecuteContext &context) const {
	if (connection_name.empty()) {
		if (context.is_parallel) {
			D_ASSERT(context.con);

			auto &test_config = TestConfiguration::Get();
			auto init_cmd = test_config.OnConnectionCommand();
			if (!init_cmd.empty()) {
				auto res = context.con->Query(runner.ReplaceKeywords(init_cmd));
				if (res->HasError()) {
					string error_msg = "Startup queries provided via on_new_connection failed: " + res->GetError();
					if (context.is_parallel) {
						throw std::runtime_error(error_msg);
					} else {
						TEST_FAIL(error_msg);
					}
				}
			}

			return *context.con;
		}
		D_ASSERT(!context.con);

		return *runner.con;
	} else {
		if (context.is_parallel) {
			throw std::runtime_error("Named connections not supported in parallel loop");
		}
		return GetConnection(runner, runner.named_connection_map, connection_name);
	}
}

bool CanRestart(Connection &conn) {
	auto &connection_manager = conn.context->db->GetConnectionManager();
	auto &db_manager = DatabaseManager::Get(*conn.context->db);
	auto &connection_list = connection_manager.GetConnectionListReference();

	// do we have any databases attached (aside from the main database)?
	auto databases = db_manager.GetDatabases();
	idx_t database_count = 0;
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		if (db.IsSystem()) {
			continue;
		}
		database_count++;
	}
	if (database_count > 1) {
		return false;
	}
	for (auto &conn_ref : connection_list) {
		auto &conn = conn_ref.first.get();
		// do we have any prepared statements?
		if (!conn.client_data->prepared_statements.empty()) {
			return false;
		}
		// we are currently inside a transaction?
		if (conn.transaction.HasActiveTransaction()) {
			return false;
		}
		// do we have any temporary objects?
		auto &temp = conn.client_data->temporary_objects;
		auto &temp_catalog = temp->GetCatalog().Cast<DuckCatalog>();
		vector<reference<DuckSchemaEntry>> schemas;
		temp_catalog.ScanSchemas(VisibilityBound::AllCommitted(),
		                         [&](DuckSchemaEntry &schema) { schemas.push_back(schema); });
		if (schemas.size() != 1) {
			return false;
		}
		auto &temp_schema = schemas[0].get();
		vector<CatalogType> catalog_types {CatalogType::TABLE_ENTRY,     CatalogType::VIEW_ENTRY,
		                                   CatalogType::INDEX_ENTRY,     CatalogType::SEQUENCE_ENTRY,
		                                   CatalogType::COLLATION_ENTRY, CatalogType::TYPE_ENTRY,
		                                   CatalogType::MACRO_ENTRY,     CatalogType::TABLE_MACRO_ENTRY};
		bool found_temp_object = false;
		for (auto &catalog_type : catalog_types) {
			temp_schema.Scan(catalog_type, VisibilityBound::AllCommitted(), [&](CatalogEntry &entry) {
				if (entry.internal) {
					return;
				}
				found_temp_object = true;
			});
			if (found_temp_object) {
				return false;
			}
		}
	}
	return true;
}

void Command::RestartDatabase(ExecuteContext &context, reference<Connection> &connection,
                              const string &sql_query) const {
	if (context.is_parallel) {
		// cannot restart in parallel
		return;
	}
	bool query_fail = false;
	try {
		connection.get().ExtractStatements(sql_query, false);
	} catch (...) {
		query_fail = true;
	}
	bool can_restart = CanRestart(connection.get());
	if (!query_fail && can_restart && !runner.skip_reload && !runner.dbpath.empty()) {
		// We basically restart the database if no transaction is active and if the query is valid
		auto command = make_uniq<RestartCommand>(runner, true);
		runner.ExecuteCommand(std::move(command));
		connection = CommandConnection(context);
	}
}

#ifdef DUCKDB_ALTERNATIVE_VERIFY
//! Drain a submitted query through the non-blocking consumer API, so the alternative-verify job
//! exercises the stream end to end. A statement that expands into several has no metadata before its result
//! statement is bound, so the metadata is read only once a row or the end of the stream arrived
static unique_ptr<QueryResult> DrainStream(unique_ptr<QueryResult> handle) {
	auto client_properties = handle->client_properties;
	QueryResultStream<> stream(std::move(handle));
	unique_ptr<ColumnDataCollection> collection;
	ColumnDataAppendState append_state;
	auto start_collection = [&]() {
		collection = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), stream.GetTypes());
		collection->InitializeAppend(append_state);
	};
	while (true) {
		unique_ptr<DataChunk> chunk;
		auto state = stream.TryFetch(chunk);
		if (state == QueryResultState::READY) {
			if (!collection) {
				start_collection();
			}
			collection->Append(append_state, *chunk);
			continue;
		}
		if (IsTerminal(state)) {
			break;
		}
		if (stream.ExecuteTask() == QueryResultState::BLOCKED) {
			stream.WaitForTask();
		}
	}
	if (stream.HasError()) {
		return make_uniq<QueryResult>(stream.GetErrorObject());
	}
	if (!collection) {
		start_collection();
	}
	return make_uniq<QueryResult>(stream.GetStatementType(), stream.GetStatementProperties(), stream.GetNames(),
	                              std::move(collection), client_properties);
}
#endif

unique_ptr<QueryResult> Command::ExecuteQuery(ExecuteContext &context, reference<Connection> connection,
                                              string file_name, idx_t query_line) const {
	query_break(query_line);

	if (TestConfiguration::TestForceReload() && TestConfiguration::TestForceStorage()) {
		RestartDatabase(context, connection, context.sql_query);
	}

	QueryParameters parameters;
	parameters.format = ChunkFormat::BufferManaged();

	QueryWatchdog watchdog(connection.get(), query_timeout);
	duckdb::unique_ptr<QueryResult> result;
	try {
#ifdef DUCKDB_ALTERNATIVE_VERIFY
		auto ccontext = connection.get().context;
		// A submission takes a single statement, and only the engine's own parse is profiled, so the
		// text is counted here and submitted as text. A parse failure takes the blocking path, which
		// reports it with its location and type, and runs text that only parses after a LOAD
		idx_t statement_count = 0;
		try {
			statement_count = connection.get().ExtractStatements(context.sql_query, false).size();
		} catch (std::exception &) {
		}
		if (statement_count != 1) {
			result = ccontext->Query(context.sql_query, parameters);
		} else {
			auto handle = ccontext->Submit(context.sql_query, parameters);
			if (handle->HasError()) {
				result = std::move(handle);
			} else if (handle->MetadataAvailable() &&
			           handle->GetStatementProperties().return_type != StatementReturnType::QUERY_RESULT) {
				handle->Complete();
				result = std::move(handle);
			} else {
				result = DrainStream(std::move(handle));
			}
		}
#else
		result = connection.get().context->Query(context.sql_query, parameters);
#endif
	} catch (std::exception &ex) {
		result = make_uniq<QueryResult>(ErrorData(ex));
	}
	if (watchdog.Finish() && result && result->HasError() &&
	    result->GetErrorObject().Type() == ExceptionType::INTERRUPT) {
		// A timeout must fail the test instead of surfacing as a SQL error: a statement expecting an error
		// would otherwise silently accept a hang bounded by the watchdog.
		auto message = StringUtil::Format("query_timeout of %.3f seconds exceeded", query_timeout);
		if (context.is_parallel) {
			// A Catch assertion would be swallowed into "Unknown error message" by the worker's catch-all.
			// The dedicated exception carries the failing statement's location for the failure bookkeeping.
			runner.finished_processing_file = true;
			throw QueryTimeoutException(message, file_name, NumericCast<int>(query_line));
		}
		runner.test_failure_locator = StringUtil::Format("%s:%llu", file_name, query_line);
		TEST_FAIL_LINE(file_name, query_line, message);
	}
	return result;
}

bool CheckLoopCondition(ExecuteContext &context, const vector<Condition> &conditions) {
	if (conditions.empty()) {
		// no conditions
		return true;
	}
	if (context.running_loops.empty()) {
		throw BinderException("Conditions (onlyif/skipif) on loop parameters can only occur within a loop");
	}
	for (auto &condition : conditions) {
		bool condition_holds = false;
		bool found_loop = false;
		for (auto &loop : context.running_loops) {
			if (loop.loop_iterator_name != condition.keyword) {
				continue;
			}
			found_loop = true;

			string loop_value;
			if (loop.tokens.empty()) {
				loop_value = to_string(loop.loop_idx);
			} else {
				loop_value = loop.tokens[loop.loop_idx];
			}
			if (condition.comparison == ExpressionType::COMPARE_EQUAL ||
			    condition.comparison == ExpressionType::COMPARE_NOTEQUAL) {
				// equality/non-equality is done on the string value
				if (condition.comparison == ExpressionType::COMPARE_EQUAL) {
					condition_holds = loop_value == condition.value;
				} else {
					condition_holds = loop_value != condition.value;
				}
			} else {
				// > >= < <= are done on numeric values
				int64_t loop_val = std::stoll(loop_value);
				int64_t condition_val = std::stoll(condition.value);
				switch (condition.comparison) {
				case ExpressionType::COMPARE_GREATERTHAN:
					condition_holds = loop_val > condition_val;
					break;
				case ExpressionType::COMPARE_LESSTHAN:
					condition_holds = loop_val < condition_val;
					break;
				case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
					condition_holds = loop_val >= condition_val;
					break;
				case ExpressionType::COMPARE_LESSTHANOREQUALTO:
					condition_holds = loop_val <= condition_val;
					break;
				default:
					throw BinderException("Unrecognized comparison for loop condition");
				}
			}
		}
		if (!found_loop) {
			throw BinderException("Condition in onlyif/skipif not found: %s must be a loop iterator name",
			                      condition.keyword);
		}
		if (condition_holds) {
			// the condition holds
			if (condition.skip_if) {
				// skip on condition holding
				return false;
			}
		} else {
			// the condition does not hold
			if (!condition.skip_if) {
				// skip on condition not holding
				return false;
			}
		}
	}
	// all conditions pass - execute
	return true;
}

void Command::Execute(ExecuteContext &context) const {
	if (runner.finished_processing_file) {
		return;
	}
	if (!CheckLoopCondition(context, conditions)) {
		// a loop-variable onlyif/skipif excludes this statement — a per-execution skip, counted like
		// a mode-skip so the tally accounts for it instead of dropping it
		if (IsCountableStatement()) {
			runner.CountSkipMode();
		}
		return;
	}
	if (context.running_loops.empty()) {
		context.sql_query = base_sql_query;
	} else {
		// perform the string replacement
		context.sql_query = runner.LoopReplacement(base_sql_query, context.running_loops);
	}
	// Per-statement outcome events (--emit-test-events): count + emit pass/fail for the countable
	// command kinds (statement/query); infrastructure commands run untracked. A failing statement
	// throws (Catch FAIL) — record the fail, then rethrow so the failure still propagates.
	if (!IsCountableStatement()) {
		ExecuteInternal(context);
		return;
	}
	// In parallel (concurrentloop) execution a failing statement does NOT throw — the worker records
	// the failure into the context (error_file) and unwinds normally, to be re-raised after join. So
	// besides the throwing (serial) path, treat a newly-set error_file as a fail; otherwise a
	// concurrent failure is miscounted as a pass.
	const bool had_error = !context.error_file.empty();
	try {
		ExecuteInternal(context);
	} catch (...) {
		runner.CountStatement(false);
		throw;
	}
	const bool newly_failed = !had_error && !context.error_file.empty();
	runner.CountStatement(!newly_failed);
}

Statement::Statement(SQLLogicTestRunner &runner) : Command(runner) {
}

Query::Query(SQLLogicTestRunner &runner) : Command(runner) {
}

ResetLabel::ResetLabel(SQLLogicTestRunner &runner) : Command(runner) {
}

void ResetLabel::ExecuteInternal(ExecuteContext &context) const {
	runner.hash_label_map.WithLock([&](unordered_map<string, CachedLabelData> &map) {
		auto it = map.find(query_label);
		//! should we allow this to be missing at all?
		if (it == map.end()) {
			TEST_FAIL_LINE(file_name, query_line, "");
		}
		map.erase(it);
	});
}

RestartCommand::RestartCommand(SQLLogicTestRunner &runner, bool load_extensions_p)
    : Command(runner), load_extensions(load_extensions_p) {
}

ReconnectCommand::ReconnectCommand(SQLLogicTestRunner &runner) : Command(runner) {
}

LoopCommand::LoopCommand(SQLLogicTestRunner &runner, LoopDefinition definition_p)
    : Command(runner), definition(std::move(definition_p)) {
}

ContinueCommand::ContinueCommand(SQLLogicTestRunner &runner) : Command(runner) {
}

ModeCommand::ModeCommand(SQLLogicTestRunner &runner, string parameter_p)
    : Command(runner), parameter(std::move(parameter_p)) {
}

SleepCommand::SleepCommand(SQLLogicTestRunner &runner, idx_t duration, SleepUnit unit)
    : Command(runner), duration(duration), unit(unit) {
}

UnzipCommand::UnzipCommand(SQLLogicTestRunner &runner, string &input, string &output)
    : Command(runner), input_path(input), extraction_path(output) {
}

LoadCommand::LoadCommand(SQLLogicTestRunner &runner, string dbpath_p, bool readonly, const string &version)
    : Command(runner), dbpath(std::move(dbpath_p)), readonly(readonly), version(version) {
}

struct ParallelExecuteContext {
	ParallelExecuteContext(SQLLogicTestRunner &runner, const vector<duckdb::unique_ptr<Command>> &loop_commands,
	                       vector<LoopDefinition> active_loops_p)
	    : runner(runner), loop_commands(loop_commands), active_loops(std::move(active_loops_p)), success(true) {
	}

	SQLLogicTestRunner &runner;
	const vector<duckdb::unique_ptr<Command>> &loop_commands;
	vector<LoopDefinition> active_loops;
	atomic<bool> success;
	string error_message;
	string error_file;
	int error_line = 0;
};

static void ParallelExecuteLoop(ParallelExecuteContext *execute_context) {
	try {
		auto &runner = execute_context->runner;

		// construct a new connection to the database
		Connection con(*runner.db);
		// create a new parallel execute context
		auto &running_loops = execute_context->active_loops;
		ExecuteContext context(con, std::move(running_loops));
		for (auto &command : execute_context->loop_commands) {
			execute_context->error_file = command->file_name;
			execute_context->error_line = command->query_line;
			command->Execute(context);
		}
		if (!context.error_file.empty()) {
			execute_context->error_message = string();
			execute_context->success = false;
			execute_context->error_file = context.error_file;
			execute_context->error_line = context.error_line;
		}
	} catch (QueryTimeoutException &ex) {
		execute_context->error_file = ex.file;
		execute_context->error_line = ex.line;
		execute_context->error_message = StringUtil::Format("Failure at %s:%d: %s", ex.file, ex.line, ex.what());
		execute_context->success = false;
	} catch (std::exception &ex) {
		execute_context->error_message = StringUtil::Format("Failure at %s:%d: %s", execute_context->error_file,
		                                                    execute_context->error_line, ex.what());
		execute_context->success = false;
	} catch (...) {
		execute_context->error_message = StringUtil::Format("Failure at %s:%d: Unknown error message",
		                                                    execute_context->error_file, execute_context->error_line);
		execute_context->success = false;
	}
	if (!execute_context->success) {
		auto message = execute_context->error_message;
		if (message.empty()) {
			message = StringUtil::Format("Failure at %s:%d", execute_context->error_file, execute_context->error_line);
		}
		SQLLogicTestRunner::EmitError(execute_context->runner.file_name, message);
	}
}

bool SQLLogicTestRunner::IsVariableReplacement(const string &token_name) {
	return StringUtil::StartsWith(token_name, "<variable:");
}

Value SQLLogicTestRunner::GetVariableReplacement(const string &token_name, string &variable_name) {
	// variable - get variable name
	auto var_size = string("<variable:").size();
	if (token_name.back() != '>') {
		throw InvalidInputException("Expected <variable:var_name>, got %s", token_name);
	}
	auto var_section = token_name.substr(var_size, token_name.size() - var_size - 1);
	auto variable_components = StringUtil::Split(var_section, ":");
	optional_ptr<Connection> var_con;
	string connection_name;
	if (variable_components.size() == 1) {
		// no connection specified
		connection_name = "main connection";
		var_con = con.get();
		variable_name = var_section;
	} else if (variable_components.size() == 2) {
		connection_name = variable_components[0];
		variable_name = variable_components[1];
		auto entry = named_connection_map.find(connection_name);
		if (entry == named_connection_map.end()) {
			throw InvalidInputException(
			    "Attempting to find variable %s in connection %s - but could not find this connection", variable_name,
			    connection_name);
		}
		var_con = entry->second.get();
	} else {
		throw InvalidInputException("Expected either <variable:variable_name> or <variable:connection:variable_name>");
	}
	Value value;
	if (!ClientConfig::GetConfig(*var_con->context).GetUserVariable(variable_name, value)) {
		throw InvalidInputException("Variable with name \"%s\" was not defined in connection %s", variable_name,
		                            connection_name);
	}
	return value;
}

bool LoopCommand::ForEachTokenReplace(const string &parameter, vector<string> &result) const {
	if (parameter.empty()) {
		return true;
	}
	auto token_name = StringUtil::Lower(parameter);
	StringUtil::Trim(token_name);
	bool collection = false;
	bool is_compression = token_name == "<compression>";
	bool is_all = token_name == "<alltypes>";
	bool is_numeric = is_all || token_name == "<numeric>";
	bool is_integral = is_numeric || token_name == "<integral>";
	bool is_signed = is_integral || token_name == "<signed>";
	bool is_unsigned = is_integral || token_name == "<unsigned>";
	bool is_all_types_column = token_name == "<all_types_columns>";
	if (token_name[0] == '!') {
		// !token tries to remove the token from the list of tokens
		auto entry = std::find(result.begin(), result.end(), parameter.substr(1));
		if (entry == result.end()) {
			// not found - insert as-is
			return false;
		}
		// found - erase the entry
		result.erase(entry);
		collection = true;
	}
	if (runner.IsVariableReplacement(token_name)) {
		string variable_name;
		auto value = runner.GetVariableReplacement(token_name, variable_name);
		if (value.IsNull()) {
			throw InvalidInputException("Variable with name \"%s\" is NULL - cannot iterate over this", variable_name);
		}
		auto list_val = value.CastAs(*runner.con->context, LogicalType::LIST(LogicalType::VARCHAR));
		for (auto &val : ListValue::GetChildren(list_val)) {
			result.push_back(StringValue::Get(val));
		}
		collection = true;
	}
	if (is_signed) {
		result.push_back("tinyint");
		result.push_back("smallint");
		result.push_back("integer");
		result.push_back("bigint");
		result.push_back("hugeint");
		collection = true;
	}
	if (is_unsigned) {
		result.push_back("utinyint");
		result.push_back("usmallint");
		result.push_back("uinteger");
		result.push_back("ubigint");
		result.push_back("uhugeint");
		collection = true;
	}
	if (is_numeric) {
		result.push_back("float");
		result.push_back("double");
		collection = true;
	}
	if (is_all) {
		result.push_back("bool");
		result.push_back("interval");
		result.push_back("varchar");
		collection = true;
	}
	if (is_compression) {
		result.push_back("none");
		result.push_back("uncompressed");
		result.push_back("rle");
		result.push_back("bitpacking");
		result.push_back("dictionary");
		result.push_back("fsst");
		result.push_back("dict_fsst");
		result.push_back("alp");
		result.push_back("alprd");
		collection = true;
	}
	if (is_all_types_column) {
		result.push_back("bool");
		result.push_back("tinyint");
		result.push_back("smallint");
		result.push_back("int");
		result.push_back("bigint");
		result.push_back("hugeint");
		result.push_back("uhugeint");
		result.push_back("utinyint");
		result.push_back("usmallint");
		result.push_back("uint");
		result.push_back("ubigint");
		result.push_back("date");
		result.push_back("time");
		result.push_back("timestamp");
		result.push_back("timestamp_s");
		result.push_back("timestamp_ms");
		result.push_back("timestamp_ns");
		result.push_back("time_tz");
		result.push_back("timestamp_tz");
		result.push_back("float");
		result.push_back("double");
		result.push_back("dec_4_1");
		result.push_back("dec_9_4");
		result.push_back("dec_18_6");
		result.push_back("dec38_10");
		result.push_back("uuid");
		result.push_back("interval");
		result.push_back("varchar");
		result.push_back("blob");
		result.push_back("bit");
		result.push_back("small_enum");
		result.push_back("medium_enum");
		result.push_back("large_enum");
		result.push_back("int_array");
		result.push_back("double_array");
		result.push_back("date_array");
		result.push_back("timestamp_array");
		result.push_back("timestamptz_array");
		result.push_back("varchar_array");
		result.push_back("nested_int_array");
		result.push_back("struct");
		result.push_back("struct_of_arrays");
		result.push_back("array_of_structs");
		result.push_back("map");
		result.push_back("union");
		result.push_back("fixed_int_array");
		result.push_back("fixed_varchar_array");
		result.push_back("fixed_nested_int_array");
		result.push_back("fixed_nested_varchar_array");
		result.push_back("fixed_struct_array");
		result.push_back("struct_of_fixed_array");
		result.push_back("fixed_array_of_int_list");
		result.push_back("list_of_fixed_int_array");
		collection = true;
	}
	return collection;
}

void LoopCommand::ExecuteInternal(ExecuteContext &context) const {
	LoopDefinition loop_def = definition;
	loop_def.loop_idx = definition.loop_start;

	if (!loop_def.tokens.empty()) {
		loop_def.tokens.clear();
		// expand any parameters in the loop definition
		for (auto &token : definition.tokens) {
			if (!ForEachTokenReplace(token, loop_def.tokens)) {
				loop_def.tokens.push_back(token);
			}
		}
		loop_def.loop_end = loop_def.tokens.size();
	}
	if (loop_def.is_parallel) {
		for (auto &running_loop : context.running_loops) {
			if (running_loop.is_parallel) {
				throw std::runtime_error("Nested parallel loop commands not allowed");
			}
		}
		for (auto &command : loop_commands) {
			if (!command->SupportsConcurrent()) {
				throw std::runtime_error("Concurrent loop is not supported over this command");
			}
		}
		vector<idx_t> loop_indexes;
		while (true) {
			loop_indexes.emplace_back(loop_def.loop_idx);
			loop_def.loop_idx++;
			if (loop_def.loop_idx >= loop_def.loop_end) {
				// finished
				break;
			}
		}
		std::random_device rd;
		std::mt19937 g(rd());
		std::shuffle(loop_indexes.begin(), loop_indexes.end(), g);

		// parallel loop: launch threads
		std::list<ParallelExecuteContext> contexts;
		for (auto &parallel_loop_idx : loop_indexes) {
			loop_def.loop_idx = parallel_loop_idx;
			auto running_loops = context.running_loops;
			running_loops.emplace_back(loop_def);
			contexts.emplace_back(runner, loop_commands, std::move(running_loops));
		}
		auto &test_config = TestConfiguration::Get();
		auto max_threads_config = test_config.GetMaxTestThreads();
		idx_t max_threads =
		    max_threads_config.IsValid() ? max_threads_config.GetIndex() : NumericLimits<idx_t>::Maximum();
		max_threads = MaxValue<idx_t>(max_threads, 1);

		std::list<std::thread> threads;
		idx_t finished_thread_idx = 0;
		auto context_it = contexts.begin();
		// the reporter is per thread: hand this test's reporter to the workers it spawns
		auto &reporter = TestReporter::Get();
		while (context_it != contexts.end()) {
			// launch threads
			for (; context_it != contexts.end() && threads.size() - finished_thread_idx < max_threads; ++context_it) {
				auto &execute_context = *context_it;
				threads.emplace_back(
				    [&reporter](ParallelExecuteContext *ctx) {
					    TestReporter::Set(reporter);
					    ParallelExecuteLoop(ctx);
				    },
				    &execute_context);
			}
			// wait for active threads to finish
			for (auto it = std::next(threads.begin(), static_cast<int64_t>(finished_thread_idx)); it != threads.end();
			     ++it) {
				it->join();
				finished_thread_idx++;
			}
		}
		for (auto &execute_context : contexts) {
			if (!execute_context.success) {
				// error_file/error_line hold the failing command's location on both the thrown and the
				// error_file paths — same source as the serial fail sites, so the locator is consistent
				runner.test_failure_locator =
				    StringUtil::Format("%s:%d", execute_context.error_file, execute_context.error_line);
				if (!execute_context.error_message.empty()) {
					TEST_FAIL(execute_context.error_message);
				} else {
					TEST_FAIL_LINE(execute_context.error_file, execute_context.error_line, "");
				}
			}
		}
	} else {
		while (!runner.finished_processing_file) {
			// execute the current iteration of the loop
			idx_t loop_index = context.running_loops.size();
			context.running_loops.push_back(loop_def);

			for (auto &statement : loop_commands) {
				statement->Execute(context);
				if (context.running_loops[loop_index].is_skipped) {
					//! Executed a CONTINUE statement
					break;
				}
			}
			context.running_loops.pop_back();
			loop_def.loop_idx++;
			if (loop_def.loop_idx >= loop_def.loop_end) {
				// finished
				break;
			}
		}
	}
}

void ContinueCommand::ExecuteInternal(ExecuteContext &context) const {
	D_ASSERT(!context.running_loops.empty());
	auto &deepest_loop = context.running_loops.back();
	deepest_loop.is_skipped = true;
}

bool ContinueCommand::SupportsConcurrent() const {
	return false;
}

bool LoopCommand::SupportsConcurrent() const {
	for (auto &command : loop_commands) {
		if (!command->SupportsConcurrent()) {
			return false;
		}
	}
	return true;
}

void Query::ExecuteInternal(ExecuteContext &context) const {
	reference<Connection> connection = CommandConnection(context);

	{
		SQLLogicTestLogger logger(context, *this);
		if (runner.output_result_mode || runner.debug_mode) {
			logger.PrintLineSep();
			logger.PrintFileHeader();
			logger.PrintSQLFormatted();
			logger.PrintLineSep();
		}

		if (runner.output_sql) {
			logger.PrintSQL();
			return;
		}
	}
	auto result = ExecuteQuery(context, connection, file_name, query_line);

	TestResultHelper helper(runner);
	if (!helper.CheckQueryResult(*this, context, std::move(result))) {
		if (context.is_parallel) {
			runner.finished_processing_file = true;
			context.error_file = file_name;
			context.error_line = query_line;
		} else {
			runner.test_failure_locator = StringUtil::Format("%s:%d", file_name, query_line);
			TEST_FAIL_LINE(file_name, query_line, "");
		}
	}
}

void RestartCommand::ExecuteInternal(ExecuteContext &context) const {
	if (context.is_parallel) {
		throw std::runtime_error("Cannot restart database in parallel");
	}
	// We save the main connection configurations to pass it to the new connection
	runner.config->options = runner.con->context->db->config.options;
	runner.config->user_settings = runner.con->context->db->config.user_settings;
	auto client_config = runner.con->context->config;
	auto catalog_search_paths = runner.con->context->client_data->catalog_search_path->GetSetPaths();
	string low_query_writer_path;
	if (runner.con->context->client_data->log_query_writer) {
		low_query_writer_path = runner.con->context->client_data->log_query_writer->path;
	}

	runner.LoadDatabase(runner.dbpath, load_extensions);

	runner.con->context->config = client_config;

	runner.con->BeginTransaction();
	runner.con->context->client_data->catalog_search_path->Set(catalog_search_paths, CatalogSetPathType::SET_SCHEMAS);
	runner.con->Commit();
	if (!low_query_writer_path.empty()) {
		runner.con->context->client_data->log_query_writer = make_uniq<BufferedFileWriter>(
		    FileSystem::GetFileSystem(*runner.con->context), low_query_writer_path, 1 << 1 | 1 << 5);
	}
}

void ReconnectCommand::ExecuteInternal(ExecuteContext &context) const {
	if (context.is_parallel) {
		throw std::runtime_error("Cannot reconnect in parallel");
	}
	runner.Reconnect();
}

void ModeCommand::ExecuteInternal(ExecuteContext &context) const {
	if (parameter == "output_hash") {
		runner.output_hash_mode = true;
	} else if (parameter == "output_result") {
		runner.output_result_mode = true;
	} else if (parameter == "no_output") {
		runner.output_hash_mode = false;
		runner.output_result_mode = false;
	} else if (parameter == "debug") {
		runner.debug_mode = true;
	} else {
		throw std::runtime_error("unrecognized mode: " + parameter);
	}
}

void SleepCommand::ExecuteInternal(ExecuteContext &context) const {
	switch (unit) {
	case SleepUnit::NANOSECOND:
		std::this_thread::sleep_for(std::chrono::duration<double, std::nano>(duration));
		break;
	case SleepUnit::MICROSECOND:
		std::this_thread::sleep_for(std::chrono::duration<double, std::micro>(duration));
		break;
	case SleepUnit::MILLISECOND:
		std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(duration));
		break;
	case SleepUnit::SECOND:
		std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(duration * 1000));
		break;
	default:
		throw std::runtime_error("Unrecognized sleep unit");
	}
}

SleepUnit SleepCommand::ParseUnit(const string &unit) {
	if (unit == "second" || unit == "seconds" || unit == "sec") {
		return SleepUnit::SECOND;
	} else if (unit == "millisecond" || unit == "milliseconds" || unit == "milli") {
		return SleepUnit::MILLISECOND;
	} else if (unit == "microsecond" || unit == "microseconds" || unit == "micro") {
		return SleepUnit::MICROSECOND;
	} else if (unit == "nanosecond" || unit == "nanoseconds" || unit == "nano") {
		return SleepUnit::NANOSECOND;
	} else {
		throw std::runtime_error("Unrecognized sleep mode - expected second/millisecond/microescond/nanosecond");
	}
}

void Statement::ExecuteInternal(ExecuteContext &context) const {
	auto &connection = CommandConnection(context);
	{
		SQLLogicTestLogger logger(context, *this);
		if (runner.output_result_mode || runner.debug_mode) {
			logger.PrintLineSep();
			logger.PrintFileHeader();
			logger.PrintSQLFormatted();
			logger.PrintLineSep();
		}

		query_break(query_line);
		if (runner.output_sql) {
			logger.PrintSQL();
			return;
		}
	}

	auto result = ExecuteQuery(context, connection, file_name, query_line);

	TestResultHelper helper(runner);
	if (!helper.CheckStatementResult(*this, context, std::move(result))) {
		if (context.is_parallel) {
			runner.finished_processing_file = true;
			context.error_file = file_name;
			context.error_line = query_line;
		} else {
			runner.test_failure_locator = StringUtil::Format("%s:%d", file_name, query_line);
			TEST_FAIL_LINE(file_name, query_line, "");
		}
	}
}

void UnzipCommand::ExecuteInternal(ExecuteContext &context) const {
	VirtualFileSystem vfs;

	// input
	FileOpenFlags in_flags(FileFlags::FILE_FLAGS_READ);
	in_flags.SetCompression(FileCompressionType::GZIP);
	auto compressed_file_handle = vfs.OpenFile(input_path, in_flags);
	if (compressed_file_handle == nullptr) {
		throw CatalogException("Cannot open the file \"%s\"", input_path);
	}

	// output
	FileOpenFlags out_flags(FileOpenFlags::FILE_FLAGS_FILE_CREATE | FileOpenFlags::FILE_FLAGS_WRITE);
	auto output_file = vfs.OpenFile(extraction_path, out_flags);
	if (!output_file) {
		throw CatalogException("Cannot open the file \"%s\"", extraction_path);
	}

	// read the compressed data from the file
	while (true) {
		duckdb::unique_ptr<char[]> compressed_buffer(new char[BUFFER_SIZE]);
		int64_t bytes_read = vfs.Read(*compressed_file_handle, compressed_buffer.get(), BUFFER_SIZE);
		if (bytes_read == 0) {
			break;
		}

		vfs.Write(*output_file, compressed_buffer.get(), bytes_read);
	}
}

void LoadCommand::ExecuteInternal(ExecuteContext &context) const {
	auto resolved_path = runner.LoopReplacement(dbpath, context.running_loops);
	if (!readonly) {
		// delete the target database file, if it exists
		DeleteDatabase(resolved_path);
	}
	runner.dbpath = resolved_path;

	// set up the config file
	if (readonly) {
		runner.config->options.use_temporary_directory = false;
		runner.config->options.access_mode = AccessMode::READ_ONLY;
	} else {
		runner.config->options.use_temporary_directory = true;
		runner.config->options.access_mode = AccessMode::AUTOMATIC;
	}
	if (runner.db) {
		if (version.empty()) {
			//! No version was provided, use the default of the main db.
			runner.config->options.storage_compatibility = runner.db->instance->config.options.storage_compatibility;
		} else {
			try {
				runner.config->options.storage_compatibility = StorageCompatibility::FromString(version);
			} catch (std::exception &ex) {
				ErrorData err(ex);
				SQLLogicTestLogger::LoadDatabaseFail(runner.file_name, dbpath, err.Message());
				TEST_FAIL("");
			}
		}
	}
	// now create the database file
	runner.LoadDatabase(resolved_path, true);
}

} // namespace duckdb
