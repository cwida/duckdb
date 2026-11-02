#include "interpreted_benchmark.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"

#include "benchmark_runner.hpp"
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "debug_fs_extension.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace duckdb {

static constexpr const char *BENCHMARK_EXTENSION_DIRECTORY_ENV = "DUCKDB_BENCHMARK_EXTENSION_DIRECTORY";

static string ParseGroupFromPath(string file) {
	string extension = "";
	// move backwards to the last slash
	int group_begin = -1, group_end = -1;
	for (size_t i = file.size(); i > 0; i--) {
		if (file[i - 1] == '/' || file[i - 1] == '\\') {
			if (group_end == -1) {
				group_end = i - 1;
			} else {
				group_begin = i;
				return "[" + file.substr(group_begin, group_end - group_begin) + "]" + extension;
			}
		}
	}
	if (group_end == -1) {
		return "[" + file + "]" + extension;
	}
	return "[" + file.substr(0, group_end) + "]" + extension;
}

struct InterpretedBenchmarkState : public BenchmarkState {
	duckdb::unique_ptr<DBConfig> benchmark_config;
	DuckDB db;
	Connection con;
	duckdb::unique_ptr<QueryResult> result;

	explicit InterpretedBenchmarkState(string path, const string &version)
	    : benchmark_config(GetBenchmarkConfig(version)),
	      db(path.empty() ? nullptr : path.c_str(), benchmark_config.get()), con(db) {
		//! Statically load the debug_fs extension so benchmarks can inject artificial I/O latency
		//! (e.g. SET GLOBAL debug_fs_delay_mean_ms=...), mirroring how the unittest runner loads it.
		db.LoadStaticExtension<DebugFsExtension>();
		auto &instance = BenchmarkRunner::GetInstance();
		auto res = con.Query("PRAGMA threads=" + to_string(instance.threads));
		D_ASSERT(!res->HasError());
		if (!instance.memory_limit.empty()) {
			res = con.Query("PRAGMA memory_limit='" + instance.memory_limit + "'");
			D_ASSERT(!res->HasError());
		}
	}

	duckdb::unique_ptr<DBConfig> GetBenchmarkConfig(const string &version = "") {
		auto result = make_uniq<DBConfig>();
		if (!version.empty()) {
			result->options.storage_compatibility = StorageCompatibility::FromString(version);
		}
		result->options.load_extensions = false;
		auto extension_directory = std::getenv(BENCHMARK_EXTENSION_DIRECTORY_ENV);
		if (extension_directory && extension_directory[0]) {
			result->SetOptionByName("allow_unsigned_extensions", true);
		}
		return result;
	}
};

void ProcessReplacements(string &str, const unordered_map<std::string, std::string> &replacement_map) {
	for (auto &replacement : replacement_map) {
		str = StringUtil::Replace(str, "${" + replacement.first + "}", replacement.second);
	}
}

struct BenchmarkFileReader {
	BenchmarkFileReader(string path_, const unordered_map<std::string, std::string> &replacement_map)
	    : path(path_), infile(path), linenr(0), replacements(replacement_map) {
	}

public:
	bool ReadLine(std::string &line) {
		if (!std::getline(infile, line)) {
			return false;
		}
		linenr++;
		ProcessReplacements(line, replacements);
		StringUtil::Trim(line);
		return true;
	}

	int LineNumber() {
		return linenr;
	}

	std::string FormatException(string exception_msg) {
		return path + ":" + std::to_string(linenr) + " - " + exception_msg;
	}

private:
	std::string path;
	std::ifstream infile;
	int linenr;
	const unordered_map<std::string, std::string> &replacements;
};

InterpretedBenchmark::InterpretedBenchmark(string full_path)
    : Benchmark(true, full_path, ParseGroupFromPath(full_path)), benchmark_path(full_path) {
	replacement_mapping["BENCHMARK_DIR"] = BenchmarkRunner::DUCKDB_BENCHMARK_DIRECTORY;
}

BenchmarkQuery InterpretedBenchmark::ReadQueryFromFile(BenchmarkFileReader &reader, string file) {
	// read the results from the file
	BenchmarkQuery query;
	query.query = "";

	ProcessReplacements(file, replacement_mapping);

	DuckDB db;
	Connection con(db);
	auto result = con.Query("FROM read_csv('" + file +
	                        "', delim='|', header=1, nullstr='NULL', all_varchar=1, quote ='\"', escape ='\"')");
	query.column_count = result->ColumnCount();
	for (auto &row : *result) {
		vector<string> row_values;
		for (idx_t col_idx = 0; col_idx < result->ColumnCount(); col_idx++) {
			row_values.push_back(row.GetValue<string>(col_idx));
		}
		query.expected_result.push_back(std::move(row_values));
	}
	return query;
}

BenchmarkQuery InterpretedBenchmark::ReadQueryFromReader(BenchmarkFileReader &reader, const string &sql,
                                                         const string &header) {
	BenchmarkQuery query;
	query.query = sql;
	query.column_count = header.size();
	// keep reading results until eof
	string line;
	while (reader.ReadLine(line)) {
		if (line.empty()) {
			break;
		}
		auto result_splits = StringUtil::Split(line, "\t");
		if (result_splits.size() != query.column_count) {
			throw std::runtime_error(reader.FormatException("expected " + std::to_string(result_splits.size()) +
			                                                " values but got " + std::to_string(query.column_count)));
		}
		query.expected_result.push_back(std::move(result_splits));
	}
	return query;
}

static void ThrowResultModeError(BenchmarkFileReader &reader) {
	vector<string> valid_options = {"streaming", "arrow", "arrow_stream", "materialized"};
	auto error = StringUtil::Format("Invalid argument for resultmode, valid options are: %s",
	                                StringUtil::Join(valid_options, ", "));
	throw std::runtime_error(reader.FormatException(error));
}

void InterpretedBenchmark::AddExtension(const string &extension, bool load_only) {
	auto &map = load_only ? load_extensions_map : extensions_map;
	auto it = map.find(extension);
	if (it != map.end()) {
		return;
	}
	map.insert(extension, map.size());
}

void InterpretedBenchmark::ProcessFile(const string &path) {
	BenchmarkFileReader reader(path, replacement_mapping);
	string line;
	while (reader.ReadLine(line)) {
		// skip blank lines and comments
		if (line.empty() || line[0] == '#') {
			continue;
		}
		// look for a command in this line
		auto splits = StringUtil::Split(StringUtil::Lower(line), ' ');
		if (splits[0] == "load" || splits[0] == "run" || splits[0] == "init" || splits[0] == "cleanup" ||
		    splits[0] == "reload") {
			if (queries.find(splits[0]) != queries.end()) {
				throw std::runtime_error("Multiple calls to " + splits[0] + " in the same benchmark file");
			}

			// load command: keep reading until we find a blank line or EOF
			string query;
			while (reader.ReadLine(line)) {
				if (line.empty()) {
					break;
				} else {
					query += line + " ";
				}
			}
			if (splits.size() > 1 && !splits[1].empty()) {
				// read entire file into query
				std::ifstream file(splits[1], std::ios::ate);
				std::streamsize size = file.tellg();
				file.seekg(0, std::ios::beg);
				if (size < 0) {
					throw std::runtime_error("Failed to read " + splits[0] + " from file " + splits[1]);
				}

				auto buffer = make_unsafe_uniq_array<char>(size);
				if (!file.read(buffer.get(), size)) {
					throw std::runtime_error("Failed to read " + splits[0] + " from file " + splits[1]);
				}
				query = string(buffer.get(), size);
			}
			StringUtil::Trim(query);
			if (query.empty()) {
				throw std::runtime_error("Encountered an empty " + splits[0] + " node!");
			}
			queries[splits[0]] = query;
		} else if (splits[0] == "require") {
			if (splits.size() < 2 || splits.size() > 3) {
				throw std::runtime_error(reader.FormatException("require requires a single parameter"));
			}
			if (splits.size() == 3) {
				if (splits[2] != "load_only") {
					throw std::runtime_error(
					    reader.FormatException("require only supports load_only as a second parameter"));
				}
				AddExtension(splits[1], true);
			} else {
				AddExtension(splits[1], false);
			}
		} else if (splits[0] == "resultmode") {
			if (splits.size() < 2) {
				ThrowResultModeError(reader);
			}
			if (splits[1] == "streaming") {
				if (splits.size() > 3) {
					throw std::runtime_error(
					    reader.FormatException("resultmode 'streaming' accepts one optional drain mode"));
				}
				if (splits.size() == 3 && splits[2] != "drop" && splits[2] != "materialize") {
					throw std::runtime_error(
					    reader.FormatException("resultmode 'streaming' drain mode must be 'materialize' or 'drop'"));
				}
				discard_stream_result = splits.size() == 3 && splits[2] == "drop";
				result_mode = BenchmarkResultMode::STREAMING;
			} else if (splits[1] == "arrow" || splits[1] == "arrow_stream") {
				arrow_batch_size = STANDARD_VECTOR_SIZE;
				if (splits.size() == 3) {
					auto custom_batch_size = std::stoi(splits[2]);
					arrow_batch_size = custom_batch_size;
				}
				if (splits.size() != 2 && splits.size() != 3) {
					throw std::runtime_error(reader.FormatException(StringUtil::Format(
					    "resultmode '%s' only takes 1 optional extra parameter (batch_size)", splits[1])));
				}
				result_mode = splits[1] == "arrow" ? BenchmarkResultMode::ARROW : BenchmarkResultMode::ARROW_STREAM;
			} else if (splits[1] == "materialized") {
				if (splits.size() != 2) {
					throw std::runtime_error(
					    reader.FormatException("resultmode 'materialized' does not accept a parameter"));
				}
				result_mode = BenchmarkResultMode::RETAINED;
			} else {
				ThrowResultModeError(reader);
			}
		} else if (splits[0] == "cache") {
			if (splits.size() == 2) {
				cache_db = splits[1];
			} else if (splits.size() == 3 && splits[2] == "no_connect") {
				cache_db = splits[1];
				cache_no_connect = true;
			} else {
				throw std::runtime_error(
				    reader.FormatException("cache requires a db file, and optionally a no_connect"));
			}
			if (StringUtil::EndsWith(cache_db, ".csv") || StringUtil::EndsWith(cache_db, ".parquet") ||
			    StringUtil::EndsWith(cache_db, ".csv.gz")) {
				cache_file = cache_db;
				cache_db = string();
			}

			ProcessReplacements(cache_db, replacement_mapping);
			ProcessReplacements(cache_file, replacement_mapping);
		} else if (splits[0] == "cache_file") {
			if (splits.size() == 2) {
				cache_file = splits[1];
				ProcessReplacements(cache_file, replacement_mapping);
			} else {
				throw std::runtime_error(reader.FormatException("cache_file requires a single file"));
			}
		} else if (splits[0] == "storage") {
			if (splits.size() < 2) {
				throw std::runtime_error(reader.FormatException("storage requires at least one parameter"));
			}
			if (splits[1] == "transient") {
				in_memory = true;
			} else if (splits[1] == "persistent") {
				in_memory = false;
			} else {
				throw std::runtime_error(reader.FormatException("Invalid argument for storage"));
			}

			if (splits.size() == 3) {
				storage_version = splits[2];
			}
		} else if (splits[0] == "require_reinit") {
			if (splits.size() != 1) {
				throw std::runtime_error(reader.FormatException("require_reinit does not take any parameters"));
			}
			require_reinit = true;
		} else if (splits[0] == "name" || splits[0] == "group" || splits[0] == "subgroup") {
			if (splits.size() == 1) {
				throw std::runtime_error(reader.FormatException(splits[0] + " requires a parameter"));
			}
			string result = line.substr(splits[0].size() + 1, line.size() - 1);
			StringUtil::Trim(result);
			if (splits[0] == "name") {
				display_name = result;
			} else if (splits[0] == "group") {
				display_group = result;
			} else {
				subgroup = result;
			}
		} else if (splits[0] == "assert") {
			// count the amount of columns
			if (splits.size() <= 1 || splits[1].size() == 0) {
				throw std::runtime_error(
				    reader.FormatException("assert must be followed by a column count (e.g. result III)"));
			}

			// read the actual query
			bool found_end = false;
			string sql;
			while (reader.ReadLine(line)) {
				if (line == "----") {
					found_end = true;
					break;
				}
				sql += "\n" + line;
			}
			if (!found_end) {
				throw std::runtime_error(reader.FormatException(
				    "result_query must be followed by a query and a result (separated by ----)"));
			}

			assert_queries.push_back(ReadQueryFromReader(reader, sql, splits[1]));
		} else if (splits[0] == "result_query" || splits[0] == "result") {
			// count the amount of columns
			if (splits.size() <= 1 || splits[1].empty()) {
				throw std::runtime_error(
				    reader.FormatException("result must be followed by a column count (e.g. result III)"));
			}
			bool is_file = false;
			for (idx_t i = 0; i < splits[1].size(); i++) {
				if (splits[1][i] != 'i') {
					is_file = true;
					break;
				}
			}
			bool matches_condition = true;
			if (splits.size() > 2) {
				// conditional result
				for (idx_t split_idx = 2; split_idx < splits.size(); split_idx++) {
					auto &condition = splits[split_idx];
					if (!StringUtil::Contains(condition, "=")) {
						throw InvalidInputException("result with condition - only = is supported currently");
					}
					auto condition_splits = StringUtil::Split(condition, '=');
					if (condition_splits.size() != 2) {
						throw InvalidInputException("result with condition must have one equality");
					}
					auto &condition_arg = condition_splits[0];
					auto &condition_val = condition_splits[1];
					auto entry = replacement_mapping.find(condition_arg);
					if (entry == replacement_mapping.end()) {
						throw InvalidInputException("Condition argument %s not found in benchmark", condition_arg);
					}
					if (entry->second != condition_val) {
						matches_condition = false;
						break;
					}
				}
			}
			string result_query;
			if (splits[0] == "result_query") {
				// read the actual query
				bool found_end = false;
				string sql;
				while (reader.ReadLine(line)) {
					if (line == "----") {
						found_end = true;
						break;
					}
					sql += "\n" + line;
				}
				if (!found_end) {
					throw std::runtime_error(reader.FormatException(
					    "result_query must be followed by a query and a result (separated by ----)"));
				}
				result_query = sql;
			} else {
				//! Read directly from the answer
				result_query = "select * from __answer";
			}
			BenchmarkQuery result_check;
			if (is_file) {
				if (matches_condition) {
					result_check = ReadQueryFromFile(reader, splits[1]);
					result_check.query = result_query;
				}
			} else {
				result_check = ReadQueryFromReader(reader, result_query, splits[1]);
			}
			if (matches_condition) {
				if (!result_queries.empty()) {
					throw std::runtime_error(reader.FormatException("multiple results found"));
				}
				result_queries.push_back(std::move(result_check));
			}
		} else if (splits[0] == "retry") {
			if (splits.size() != 3) {
				throw std::runtime_error(reader.FormatException(splits[0] + " requires two parameters"));
			}
			if (splits[1] != "load") {
				throw std::runtime_error("Only retry load is supported");
			}
			retry_load = std::stoull(splits[2]);
		} else if (splits[0] == "template") {
			// template: update the path to read
			benchmark_path = splits[1];
			// now read parameters
			while (reader.ReadLine(line)) {
				if (line.empty()) {
					break;
				}
				auto parameters = StringUtil::Split(line, '=');
				if (parameters.size() != 2) {
					throw std::runtime_error(
					    reader.FormatException("Expected a template parameter in the form of X=Y"));
				}
				replacement_mapping[parameters[0]] = parameters[1];
			}
			// restart the load from the template file
			LoadBenchmark();
			return;
		} else if (splits[0] == "argument") {
			if (splits.size() != 3) {
				throw std::runtime_error(
				    reader.FormatException(splits[0] + " requires two parameters (name and default)"));
			}
			auto &arg_name = splits[1];
			string arg_value = splits[2];
			auto &instance = BenchmarkRunner::GetInstance();
			auto entry = instance.custom_arguments.find(arg_name);
			if (entry != instance.custom_arguments.end()) {
				arg_value = entry->second;
			}
			if (handled_arguments.count(arg_name) > 0) {
				// argument is already defined - ignore this definition
				continue;
			}
			handled_arguments.insert(arg_name);
			replacement_mapping[arg_name] = std::move(arg_value);
		} else if (splits[0] == "include") {
			if (splits.size() != 2) {
				throw InvalidInputException("include requires a single argument");
			}
			ProcessFile(splits[1]);
		} else {
			throw std::runtime_error(reader.FormatException("unrecognized command " + splits[0]));
		}
	}
}

void InterpretedBenchmark::LoadBenchmark() {
	if (is_loaded) {
		return;
	}

	ProcessFile(benchmark_path);
	// throw an error if an argument was not handled
	auto &instance = BenchmarkRunner::GetInstance();
	for (auto &entry : instance.custom_arguments) {
		auto &custom_arg = entry.first;
		if (handled_arguments.count(custom_arg) == 0) {
			throw InvalidInputException("Invalid benchmark argument %s: argument was not specified in benchmark %s",
			                            custom_arg, benchmark_path);
		}
	}
	// set up the queries
	if (queries.find("run") == queries.end()) {
		throw InvalidInputException("Invalid benchmark file: no \"run\" query specified");
	}
	run_query = queries["run"];
	if (discard_stream_result && !result_queries.empty()) {
		throw InvalidInputException(
		    "Invalid benchmark file: resultmode 'streaming drop' discards the result and cannot verify it");
	}
	is_loaded = true;
}

void InterpretedBenchmark::LoadExtensions(InterpretedBenchmarkState &state, bool is_load_set) {
	auto &map = is_load_set ? load_extensions_map : extensions_map;
	for (auto &it : map) {
		auto &extension = it.first;
		auto result = ExtensionHelper::LoadExtension(state.db, extension);
		if (result == ExtensionLoadResult::EXTENSION_UNKNOWN) {
			throw InvalidInputException("Unknown extension " + extension);
		} else if (result == ExtensionLoadResult::NOT_LOADED) {
			auto extension_directory = std::getenv(BENCHMARK_EXTENSION_DIRECTORY_ENV);
			if (extension_directory && extension_directory[0]) {
				auto fs = FileSystem::CreateLocal();
				auto extension_path = fs->JoinPath(extension_directory, extension + ".duckdb_extension");
				if (!fs->FileExists(extension_path)) {
					throw InvalidInputException("Extension %s is not linked and was not found at %s", extension,
					                            extension_path);
				}
				auto load_result = state.con.Query("LOAD " + SQLString(extension_path));
				if (load_result->HasError()) {
					throw InvalidInputException("Failed to load benchmark extension %s from %s: %s", extension,
					                            extension_path, load_result->GetError());
				}
				continue;
			}
			throw InvalidInputException("Extension " + extension +
			                            " is not available/was not compiled. Cannot run this benchmark.");
		}
	}
}

unique_ptr<QueryResult> InterpretedBenchmark::RunLoadQuery(InterpretedBenchmarkState &state, const string &load_query) {
	LoadExtensions(state, true);
	auto result = state.con.Query(load_query);
	for (idx_t i = 0; i < retry_load; i++) {
		if (!result->HasError()) {
			break;
		}
		result = state.con.Query(load_query);
	}
	return result;
}

unique_ptr<BenchmarkState> InterpretedBenchmark::Initialize(BenchmarkConfiguration &config) {
	duckdb::unique_ptr<QueryResult> result;
	LoadBenchmark();
	duckdb::unique_ptr<InterpretedBenchmarkState> state;
	auto full_db_path = GetDatabasePath();
	try {
		state = make_uniq<InterpretedBenchmarkState>(full_db_path, storage_version);
	} catch (Exception &e) {
		// if the connection throws an error, chances are it's a storage format error.
		// In this case delete the file and connect again.
		DeleteDatabase(full_db_path);
		state = make_uniq<InterpretedBenchmarkState>(full_db_path, storage_version);
	}
	AddExtension("core_functions", false);
	AddExtension("parquet", false);

	LoadExtensions(*state, false);
	if (queries.find("init") != queries.end()) {
		string init_query = queries["init"];
		result = state->con.Query(init_query);
		while (result) {
			result->ThrowIfError();
			result = std::move(result->next);
		}
	}

	string load_query;
	if (queries.find("load") != queries.end()) {
		load_query = queries["load"];
	}
	string reload_query;
	if (queries.find("reload") != queries.end()) {
		reload_query = queries["reload"];
	}

	if (!cache_file.empty()) {
		auto fs = FileSystem::CreateLocal();
		if (!fs->FileExists(fs->JoinPath(BenchmarkRunner::DUCKDB_BENCHMARK_DIRECTORY, cache_file))) {
			// no cache or db_path specified: just run the initialization code
			result = RunLoadQuery(*state, load_query);
		} else if (!reload_query.empty()) {
			// run reload query
			result = RunLoadQuery(*state, reload_query);
		}
	} else if (cache_db.empty() && cache_db.compare(DEFAULT_DB_PATH) != 0) {
		// no cache or db_path specified: just run the initialization code
		result = RunLoadQuery(*state, load_query);
	} else {
		// cache or db_path is specified: try to load from one of them
		bool in_memory_db_has_data = false;
		if (!cache_db.empty()) {
			// Currently connected to a cached db. check if any tables exist.
			// If tables exist, it's a good indication that the database is fine
			// If they don't load the database
			auto result = state->con.Query("SHOW TABLES;");
			result->ThrowIfError();
			if (result->RowCount() > 0) {
				in_memory_db_has_data = true;
			}
		}
		if (!in_memory_db_has_data) {
			// failed to load: write the cache
			result = RunLoadQuery(*state, load_query);
		} else if (!reload_query.empty()) {
			// succeeded: run the reload query
			result = RunLoadQuery(*state, reload_query);
		}
	}
	while (result) {
		result->ThrowIfError();
		result = std::move(result->next);
	}

	// if a cache db is required but no connection, then reset the connection
	if (!cache_db.empty() && cache_no_connect) {
		cache_db = "";
		in_memory = true;
		cache_no_connect = false;
		if (!load_query.empty()) {
			queries.erase("load");
		}
		return Initialize(config);
	}

	if (config.profile_info == BenchmarkProfileInfo::NORMAL) {
		state->con.Query("PRAGMA enable_profiling");
	} else if (config.profile_info == BenchmarkProfileInfo::DETAILED) {
		state->con.Query("PRAGMA enable_profiling");
		state->con.Query("PRAGMA profiling_mode='detailed'");
	}
	return std::move(state);
}

string InterpretedBenchmark::GetQuery() {
	LoadBenchmark();
	return run_query;
}

void InterpretedBenchmark::Assert(BenchmarkState *state_p) {
	auto &state = (InterpretedBenchmarkState &)*state_p;

	for (auto &assert_query : assert_queries) {
		auto &query = assert_query.query;
		auto result = state.con.Query(query);
		result->ThrowIfError();
		auto verify_result = VerifyInternal(state_p, assert_query, *result);
		if (!verify_result.empty()) {
			throw InvalidInputException("Assertion query failed:\n%s", verify_result);
		}
	}
}

void InterpretedBenchmark::Run(BenchmarkState *state_p) {
	auto &state = (InterpretedBenchmarkState &)*state_p;
	auto &context = state.con.context;

	if (result_mode == BenchmarkResultMode::ARROW_STREAM) {
		auto handle = context->Submit(run_query, make_shared_ptr<ArrowFormat>(arrow_batch_size));
		if (handle->HasError()) {
			state.result = std::move(handle);
			return;
		}
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		while (stream.Fetch()) {
		}
		state.result = stream.HasError() ? make_uniq<QueryResult>(stream.GetErrorObject()) : nullptr;
		return;
	}
	if (result_mode == BenchmarkResultMode::STREAMING) {
		auto handle = context->Submit(run_query, QueryParameters());
		if (handle->HasError()) {
			state.result = std::move(handle);
			return;
		}
		auto statement_type = handle->GetStatementType();
		auto properties = handle->GetStatementProperties();
		auto names = handle->GetNames();
		auto client_properties = handle->client_properties;
		QueryResultStream<> stream(std::move(handle));
		unique_ptr<ColumnDataCollection> collection;
		ColumnDataAppendState append_state;
		if (!discard_stream_result) {
			collection = make_uniq<ColumnDataCollection>(*context, stream.GetTypes(),
			                                             ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
			collection->InitializeAppend(append_state);
		}
		while (auto chunk = stream.Fetch()) {
			if (chunk->size() == 0) {
				break;
			}
			if (collection) {
				collection->Append(append_state, *chunk);
			}
		}
		if (stream.HasError()) {
			state.result = make_uniq<QueryResult>(stream.GetErrorObject());
		} else if (collection) {
			state.result =
			    make_uniq<QueryResult>(statement_type, properties, names, std::move(collection), client_properties);
		} else {
			state.result = nullptr;
		}
		return;
	}
	QueryParameters parameters;
	if (result_mode == BenchmarkResultMode::RETAINED) {
		parameters.format = ChunkFormat::BufferManaged();
	} else if (result_mode == BenchmarkResultMode::ARROW) {
		parameters.format = make_shared_ptr<ArrowFormat>(arrow_batch_size);
	}
	auto temp_result = context->Query(run_query, parameters);
	if (result_mode == BenchmarkResultMode::ARROW) {
		if (temp_result->RowCount() > 0 && temp_result->Collection<ArrowFormat>().empty()) {
			throw InternalException("Query produced rows but no Arrow arrays");
		}
		/* no-op, this is only used to test the overhead of the conversion */
		state.result = nullptr;
		return;
	}
	state.result = std::move(temp_result);
}

void InterpretedBenchmark::Cleanup(BenchmarkState *state_p) {
	auto &state = (InterpretedBenchmarkState &)*state_p;
	if (queries.find("cleanup") != queries.end()) {
		duckdb::unique_ptr<QueryResult> result;
		string cleanup_query = queries["cleanup"];
		result = state.con.Query(cleanup_query);
		while (result) {
			result->ThrowIfError();
			result = std::move(result->next);
		}
	}
}

string InterpretedBenchmark::GetDatabasePath() {
	auto fs = FileSystem::CreateLocal();
	if (!cache_db.empty()) {
		return fs->JoinPath(BenchmarkRunner::DUCKDB_BENCHMARK_DIRECTORY, cache_db);
	}
	if (in_memory) {
		return "";
	}
	auto db_path = fs->JoinPath(BenchmarkRunner::DUCKDB_BENCHMARK_DIRECTORY, DEFAULT_DB_PATH);
	DeleteDatabase(db_path);
	return db_path;
}

string InterpretedBenchmark::VerifyInternal(BenchmarkState *state_p, const BenchmarkQuery &query, QueryResult &result) {
	auto &state = (InterpretedBenchmarkState &)*state_p;

	auto &result_values = query.expected_result;
	D_ASSERT(query.column_count >= 1);
	if (query.column_count != result.ColumnCount()) {
		return StringUtil::Format("Error in result: expected %lld columns but got %lld\nObtained result: %s",
		                          (int64_t)query.column_count, (int64_t)result.ColumnCount(), result.ToString());
	}

	// compare row count
	if (result.RowCount() != query.expected_result.size()) {
		return StringUtil::Format("Error in result: expected %lld rows but got %lld\nObtained result: %s",
		                          (int64_t)result_values.size(), (int64_t)result.RowCount(), result.ToString());
	}
	// compare values
	auto rows = result.Collection().GetRows();
	for (idx_t r = 0; r < result_values.size(); r++) {
		for (idx_t c = 0; c < query.column_count; c++) {
			auto value = rows.GetValue(c, r);
			if (result_values[r][c] == "NULL" && value.IsNull()) {
				continue;
			}
			if (result_values[r][c] == value.ToString()) {
				continue;
			}
			if (result_values[r][c] == "(empty)" && (value.ToString() == "" || value.IsNull())) {
				continue;
			}

			Value verify_val(result_values[r][c]);
			try {
				verify_val = verify_val.CastAs(*state.con.context, value.type());
			} catch (...) {
			}
			if (!Value::ValuesAreEqual(*state.con.context, verify_val, value)) {
				return StringUtil::Format("Error in result on row %lld column %lld: expected value \"%s\" but got "
				                          "value \"%s\"\nObtained result:\n%s",
				                          r + 1, c + 1, verify_val.ToString().c_str(), value.ToString().c_str(),
				                          result.ToString().c_str());
			}
		}
	}
	return string();
}

string InterpretedBenchmark::Verify(BenchmarkState *state_p) {
	auto &state = (InterpretedBenchmarkState &)*state_p;
	if (!state.result) {
		D_ASSERT(result_mode != BenchmarkResultMode::RETAINED);
		return string();
	}

	if (state.result->HasError()) {
		return state.result->GetError();
	}
	if (result_queries.empty()) {
		// no result specified
		return string();
	}
	D_ASSERT(result_queries.size() == 1);
	auto &query = result_queries[0];
	auto result_query = query.query;
	if (result_query.empty()) {
		result_query = "select * from __answer";
	}

	// we are running a result query
	// store the current result in a table called "__answer"
	auto &collection = state.result->Collection();
	auto &names = state.result->GetNames();
	auto &types = state.result->GetTypes();
	identifier_set_t name_set;
	// first create the (empty) table
	string create_tbl = "CREATE OR REPLACE TEMP TABLE __answer(";
	for (idx_t i = 0; i < names.size(); i++) {
		if (!name_set.insert(names[i]).second) {
			auto err_str = StringUtil::Format("Duplicate column name \"%s\" in benchmark query", names[i]);
			throw std::runtime_error(err_str);
		}
		if (i > 0) {
			create_tbl += ", ";
		}
		create_tbl += SQLIdentifier(names[i]);
		create_tbl += " ";
		create_tbl += types[i].ToString();
	}
	create_tbl += ")";
	auto new_result = state.con.Query(create_tbl);
	if (new_result->HasError()) {
		return new_result->GetError();
	}
	// now append the result to the answer table
	auto table_info = state.con.TableInfo("__answer");
	if (table_info == nullptr) {
		throw std::runtime_error("Received a nullptr when querying table info of __answer");
	}
	state.con.Append(*table_info, collection);

	// finally run the result query and verify the result of that query
	new_result = state.con.Query(result_query);
	if (new_result->HasError()) {
		return new_result->GetError();
	}
	return VerifyInternal(state_p, query, *new_result);
}

void InterpretedBenchmark::Interrupt(BenchmarkState *state_p) {
	auto &state = (InterpretedBenchmarkState &)*state_p;
	state.con.Interrupt();
}

string InterpretedBenchmark::BenchmarkInfo() {
	return string();
}

string InterpretedBenchmark::GetLogOutput(BenchmarkState *state_p) {
	auto &state = (InterpretedBenchmarkState &)*state_p;
	auto &profiler = QueryProfiler::Get(*state.con.context);
	return profiler.ToJSON();
}

string InterpretedBenchmark::DisplayName() {
	LoadBenchmark();
	return display_name.empty() ? name : display_name;
}

string InterpretedBenchmark::Group() {
	LoadBenchmark();
	return display_group.empty() ? group : display_group;
}

string InterpretedBenchmark::Subgroup() {
	LoadBenchmark();
	return subgroup;
}

} // namespace duckdb
