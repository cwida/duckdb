
#include "sqllogic_test_runner.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/settings.hpp"
#include "sqllogic_parser.hpp"
#include "test_helpers.hpp"
#include "test_reporter.hpp"
#include "sqllogic_test_logger.hpp"
#include "duckdb/common/random_engine.hpp"

#ifdef DUCKDB_OUT_OF_TREE
#include DUCKDB_EXTENSION_HEADER
#endif

namespace duckdb {

mutex SQLLogicTestRunner::skip_reason_lock;
map<string, idx_t> SQLLogicTestRunner::skip_reason_counts;

SQLLogicTestRunner::SQLLogicTestRunner(string dbpath) : dbpath(std::move(dbpath)), finished_processing_file(false) {
	config = GetTestConfig();
	config->SetOptionByName("allow_unredacted_secrets", true);
	config->options.load_extensions = false;
	test_dir_prefix = TestDirectoryPath();

	auto &test_config = TestConfiguration::Get();
	autoloading_mode = test_config.GetExtensionAutoLoadingMode();

	bool autoload_known_extensions = false;
	bool autoinstall_known_extensions = false;
	config->SetOptionByName("allow_unsigned_extensions", true);
	// enable custom trusted extension repositories at startup (they cannot be enabled at runtime), mirroring the
	// permissive allow_unsigned_extensions default above. Tests can still tighten this at runtime
	config->SetOptionByName("allow_extension_repositories", "allowed");
	local_extension_repo = "";
	autoinstall_is_checked = false;

	switch (autoloading_mode) {
	case TestConfiguration::ExtensionAutoLoadingMode::NONE: {
		break;
	}
	case TestConfiguration::ExtensionAutoLoadingMode::AVAILABLE: {
		autoinstall_is_checked = true;
		autoload_known_extensions = true;
		break;
	}
	case TestConfiguration::ExtensionAutoLoadingMode::ALL: {
		autoinstall_is_checked = false;
		autoload_known_extensions = true;
		autoinstall_known_extensions = true;
		break;
	}
	}

	auto repo = TestBuildDirectory() + "/repository";
	auto env_var = std::getenv("LOCAL_EXTENSION_REPO");
	if (env_var) {
		local_extension_repo = env_var;
		autoload_known_extensions = true;
		autoinstall_known_extensions = true;
	} else if (autoload_known_extensions) {
		local_extension_repo = repo;
	}
	test_config.SetLocalExtensionRepository(repo);
	config->SetOptionByName("autoinstall_known_extensions", autoinstall_known_extensions);
	config->SetOptionByName("autoload_known_extensions", autoload_known_extensions);
	for (auto &entry : test_config.GetConfigSettings()) {
		config->SetOptionByName(entry.name, entry.value);
	}
}

SQLLogicTestRunner::~SQLLogicTestRunner() {
	config.reset();
	con.reset();
	db.reset();
	// Post-test DB cleanup, gated by --database-destroy (on-success aware). Skipping here on
	// retain/failure keeps loaded DBs inside a retained temp dir (e.g. test_zero_initialize.py,
	// which diffs the generated DB files after the process exits).
	if (DatabaseDestroyFires(test_succeeded)) {
		for (auto &loaded_path : loaded_databases) {
			if (loaded_path.empty()) {
				continue;
			}
			// only delete database files that were created during the tests
			if (!StringUtil::StartsWith(loaded_path, test_dir_prefix)) {
				continue;
			}
			DeleteDatabase(loaded_path);
		}
	}
}

void SQLLogicTestRunner::AddSkipReason(const string &reason) {
	lock_guard<mutex> guard(skip_reason_lock);
	skip_reason_counts[reason]++;
}

void SQLLogicTestRunner::SkipTest(const string &reason, TestSkipKind kind) {
	AddSkipReason(reason);
	test_skip_kind = kind;
	// A whole-test skip is a terminal disposition, not a mid-stream event: record it and let the
	// Catch wrapper emit the single end {status:"skip-requirement"} terminal.
	test_skipped_requirement = true;
	test_skip_reason = reason;
	SQLLogicTestLogger::ReportSkip(file_name, reason);
}

string SQLLogicTestRunner::GetSkipReasonSummary() {
	lock_guard<mutex> guard(skip_reason_lock);
	if (skip_reason_counts.empty()) {
		return string();
	}
	std::ostringstream oss;
	for (auto &entry : skip_reason_counts) {
		oss << entry.first << ": " << entry.second << "\n";
	}
	return oss.str();
}

void SQLLogicTestRunner::CountStatement(bool passed) {
	if (passed) {
		test_stat_passes++;
	} else {
		test_stat_fails++;
	}
}

void SQLLogicTestRunner::CountSkipMode() {
	test_stat_skip_mode++;
}

void SQLLogicTestRunner::EmitBegin(const string &test_name) {
	// Snapshotted before the enabled check and before the body runs: this is the dir the harness just
	// assigned, and it must survive any test-env rewrite so end still reports what begin did.
	auto entry = environment_variables.find("TEMP_DIR");
	emit_temp_dir = entry == environment_variables.end() ? "" : entry->second;
	if (!EmitTestEventsEnabled()) {
		return;
	}
	JSONWriter writer;
	auto obj = writer.CreateObject();
	obj.AddString("event", "begin");
	// Version 2 distinguishes the test body from initialization and cleanup.
	obj.Add("version", writer.CreateUnsignedInteger(2));
	obj.AddString("name", test_name);
	// Lets a consumer verify which invocation emitted this rather than inferring it from which
	// subprocess it launched: under --temp-dir-root the prefix is what the driver passed in.
	obj.AddString("temp_dir", emit_temp_dir);
	writer.SetRoot(obj);
	SQLLogicTestLogger::EmitTestEvent(writer.ToString());
}

void SQLLogicTestRunner::EmitExecutionComplete(const string &test_name, const string &status, const string &data) {
	if (execution_complete_emitted) {
		return;
	}
	execution_complete_emitted = true;
	EmitStatusEvent("execution-complete", test_name, status, data);
}

void SQLLogicTestRunner::EmitError(const string &test_name, const string &data) {
	if (!EmitTestEventsEnabled()) {
		return;
	}
	// Worker threads must report errors before joining, without reading the runner's mutable statistics.
	JSONWriter writer;
	auto obj = writer.CreateObject();
	obj.AddString("event", "error");
	obj.AddString("name", test_name);
	obj.AddString("data", data);
	writer.SetRoot(obj);
	SQLLogicTestLogger::EmitTestEvent(writer.ToString());
}

void SQLLogicTestRunner::EmitEnd(const string &test_name, const string &status, const string &data) {
	EmitStatusEvent("end", test_name, status, data);
}

void SQLLogicTestRunner::EmitStatusEvent(const string &event, const string &test_name, const string &status,
                                         const string &data) {
	if (!EmitTestEventsEnabled()) {
		return;
	}
	JSONWriter writer;
	auto obj = writer.CreateObject();
	obj.AddString("event", event);
	obj.AddString("name", test_name);
	obj.AddString("status", status);
	obj.Add("passes", writer.CreateUnsignedInteger(test_stat_passes.load()));
	obj.Add("fails", writer.CreateUnsignedInteger(test_stat_fails.load()));
	obj.Add("skip-mode", writer.CreateUnsignedInteger(test_stat_skip_mode.load()));
	if (!partial_skip_reasons.empty()) {
		auto reasons = writer.CreateArray();
		for (auto &reason : partial_skip_reasons) {
			reasons.AppendString(reason);
		}
		obj.Add("partial-skip-reasons", reasons);
	}
	if (!data.empty()) {
		obj.AddString("data", data);
	}
	obj.AddString("temp_dir", emit_temp_dir);
	writer.SetRoot(obj);
	SQLLogicTestLogger::EmitTestEvent(writer.ToString());
}

void SQLLogicTestRunner::ExecuteCommand(duckdb::unique_ptr<Command> command) {
	if (InLoop()) {
		auto &current_loop = *active_loops.back();
		current_loop.loop_commands.push_back(std::move(command));
	} else {
		ExecuteContext context;
		command->Execute(context);
	}
}

void SQLLogicTestRunner::StartLoop(LoopDefinition definition) {
	auto loop = make_uniq<LoopCommand>(*this, std::move(definition));
	auto loop_ptr = loop.get();
	if (InLoop()) {
		auto &current_loop = *active_loops.back();
		current_loop.loop_commands.push_back(std::move(loop));
	} else {
		// not in a loop yet: new top-level loop
		top_level_loop = std::move(loop);
	}
	active_loops.push_back(loop_ptr);
}

void SQLLogicTestRunner::EndLoop() {
	// finish a loop: pop it from the active_loop queue
	if (active_loops.empty()) {
		throw std::runtime_error("endloop without active loop!");
	}
	active_loops.pop_back();
	if (active_loops.empty()) {
		// not in a loop
		ExecuteContext context;
		top_level_loop->Execute(context);
		top_level_loop.reset();
	}
}

ExtensionLoadResult SQLLogicTestRunner::LoadExtension(DuckDB &db, const std::string &extension) {
	if (db.ExtensionIsLoaded(extension)) {
		return ExtensionLoadResult::LOADED_EXTENSION;
	}

	// Prefer built-in/static extension loading first. Otherwise we can end up loading the same extension
	// from the local extension repo as well, which causes duplicate symbols in sanitizer builds.
	auto linked_result = ExtensionHelper::LoadExtension(db, extension);
	if (linked_result == ExtensionLoadResult::LOADED_EXTENSION) {
		return linked_result;
	}

	auto &test_config = TestConfiguration::Get();
	Connection con(db);
	if (test_config.GetExtensionAutoLoadingMode() == TestConfiguration::ExtensionAutoLoadingMode::NONE) {
		auto extension_path = extension;
		if (!ExtensionHelper::IsFullPath(extension)) {
			auto &fs = FileSystem::GetFileSystem(*con.context);
			auto extension_name = ExtensionHelper::ApplyExtensionAlias(extension);
			extension_path =
			    fs.JoinPath(test_config.GetLocalExtensionRepository(), ExtensionHelper::GetVersionDirectoryName());
			extension_path = fs.JoinPath(extension_path, DuckDB::Platform());
			extension_path = fs.JoinPath(extension_path, extension_name + ".duckdb_extension");
		}
		try {
			ExtensionHelper::LoadExternalExtension(*con.context, ExtensionLoadOptions(extension_path));
			return ExtensionLoadResult::LOADED_EXTENSION;
		} catch (std::exception &) {
			return linked_result;
		}
	}

	// try LOAD extension
	auto result = con.Query("LOAD " + extension);
	if (!result->HasError()) {
		return ExtensionLoadResult::LOADED_EXTENSION;
	}

	return linked_result;
}

NewDatabaseConnection SQLLogicTestRunner::CreateDatabase(const string &db_path, bool load_extensions) {
	NewDatabaseConnection result;
	// a prefixed path (ducklake:...) loads its extension inside the DuckDB constructor, before LoadExtension can run
	auto saved_extension_directories = config->options.extension_directories;
	if (autoloading_mode == TestConfiguration::ExtensionAutoLoadingMode::NONE &&
	    !ExtensionHelper::ExtractExtensionPrefixFromPath(db_path).empty()) {
		config->options.extension_directories = {TestConfiguration::Get().GetLocalExtensionRepository()};
	}
	try {
		result.db = make_uniq<DuckDB>(db_path, config.get());
		config->options.extension_directories = saved_extension_directories;
		result.db->instance->config.options.extension_directories = saved_extension_directories;
		LoadStaticExtensions(*result.db);
		ConfigureDefaultInMemoryTemporaryDirectory(*result.db, db_path);

		// always load core functions
		auto &test_config = TestConfiguration::Get();
		for (auto ext : test_config.ExtensionToBeLoadedOnLoad()) {
			SQLLogicTestRunner::LoadExtension(*result.db, ext);
		}
	} catch (std::exception &ex) {
		config->options.extension_directories = saved_extension_directories;
		ErrorData err(ex);
		SQLLogicTestLogger::LoadDatabaseFail(file_name, db_path, err.Message());
		TEST_FAIL("");
	}
	result.con = ConnectToDatabase(*result.db);
	// load any previously loaded extensions again
	if (load_extensions) {
		for (auto &extension : extensions) {
			SQLLogicTestRunner::LoadExtension(*result.db, extension);
		}
	}
	return result;
}

void SQLLogicTestRunner::LoadDatabase(string dbpath, bool load_extensions) {
	loaded_databases.push_back(dbpath);

	// restart the database with the specified db path
	db.reset();
	con.reset();
	named_connection_map.clear();
	// now re-open the current database

	auto result = CreateDatabase(dbpath, load_extensions);
	db = std::move(result.db);
	con = std::move(result.con);
}

unique_ptr<Connection> SQLLogicTestRunner::ConnectToDatabase(DuckDB &db_ref) {
	auto result = make_uniq<Connection>(db_ref);
	if (original_sqlite_test) {
		result->Query("SET integer_division=true");
		result->Query("SET error_on_division_by_zero=false");
	}
	result->Query("SET secret_directory='" + TestCreatePath("test_secret_dir") + "'");
#ifdef DUCKDB_ALTERNATIVE_VERIFY
	result->Query("SET pivot_filter_threshold=0");
#endif
	auto &client_config = ClientConfig::GetConfig(*result->context);
	client_config.enable_progress_bar = true;
	client_config.print_progress_bar = false;
	// Set the local extension repo for autoinstalling extensions
	if (!local_extension_repo.empty()) {
		auto res1 = result->Query("SET autoinstall_extension_repository='" + local_extension_repo + "'");
	}

	auto &test_config = TestConfiguration::Get();
	auto init_cmd = test_config.OnInitCommand() + ";" + test_config.OnConnectionCommand();
	if (!init_cmd.empty()) {
		test_config.ProcessPath(init_cmd, file_name);
		auto res = result->Query(ReplaceKeywords(init_cmd));
		if (res->HasError()) {
			TEST_FAIL("Startup queries provided via on_init failed: " + res->GetError());
		}
	}
	return result;
}

void SQLLogicTestRunner::Reconnect() {
	con = ConnectToDatabase(*db);
}

void StringReplaceLoopIterator(string &text, const string &loop_iterator_name, const string &replacement,
                               const string &test_name) {
	auto loop_it = "{" + loop_iterator_name + "}";
	auto deprecated_loop_it = "$" + loop_it;
	if (StringUtil::Contains(text, deprecated_loop_it)) {
		Printer::PrintF("Replacing deprecated loop iterator %s in test \"%s\" - please use the new loop iterator %s",
		                deprecated_loop_it, test_name, loop_it);
		text = StringUtil::Replace(text, deprecated_loop_it, replacement);
	}
	text = StringUtil::Replace(text, loop_it, replacement);
}

string SQLLogicTestRunner::ReplaceLoopIterator(string text, string loop_iterator_name, string replacement) {
	replacement = ReplaceKeywords(replacement);
	if (StringUtil::Contains(loop_iterator_name, ",")) {
		auto name_splits = StringUtil::Split(loop_iterator_name, ",");
		auto replacement_splits = StringUtil::Split(replacement, ",");
		if (name_splits.size() != replacement_splits.size()) {
			TEST_FAIL("foreach loop: number of commas in loop iterator (" + loop_iterator_name +
			          ") does not match number of commas in replacement (" + replacement + ")");
		}
		for (idx_t i = 0; i < name_splits.size(); i++) {
			StringReplaceLoopIterator(text, name_splits[i], replacement_splits[i], file_name);
		}
		return text;
	} else {
		StringReplaceLoopIterator(text, loop_iterator_name, replacement, file_name);
		return text;
	}
}

string SQLLogicTestRunner::LoopReplacement(string text, const vector<LoopDefinition> &loops) {
	for (auto &active_loop : loops) {
		if (active_loop.tokens.empty()) {
			// regular loop
			text = ReplaceLoopIterator(text, active_loop.loop_iterator_name, to_string(active_loop.loop_idx));
		} else {
			// foreach loop
			text = ReplaceLoopIterator(text, active_loop.loop_iterator_name, active_loop.tokens[active_loop.loop_idx]);
		}
	}
	return text;
}

string SQLLogicTestRunner::ReplaceKeywords(string input) {
	// ProcessPath replaced, can simplify this into simple `ReplaceVariables` loop.
	//
	// Replace environment variables in the SQL
	for (auto &it : environment_variables) {
		auto &name = it.first;
		auto &value = it.second;
		auto legacy_syntax = StringUtil::Format("${%s}", name);
		auto env_syntax = StringUtil::Format("{%s}", name);
		if (StringUtil::Contains(input, legacy_syntax)) {
			Printer::PrintF("Replacing deprecated %s in test %s - please replace with %s", legacy_syntax, file_name,
			                env_syntax);
			input = StringUtil::Replace(input, legacy_syntax, value);
		}
		input = StringUtil::Replace(input, env_syntax, value);
	}
	auto &test_config = TestConfiguration::Get();
	test_config.ProcessPath(input, file_name);
	input = StringUtil::Replace(input, "{BUILD_DIRECTORY}", TestBuildDirectory());
	if (StringUtil::Contains(input, "__BUILD_DIRECTORY__")) {
		Printer::PrintF("Replacing deprecated __BUILD_DIRECTORY__ in test %s - please replace with {BUILD_DIRECTORY}",
		                file_name);
		input = StringUtil::Replace(input, "__BUILD_DIRECTORY__", TestBuildDirectory());
	}

	return input;
}

static string ParseExplanation(SQLLogicParser &parser, const vector<string> &params, size_t &index) {
	string res;
	if (params[index].empty() || params[index][0] != '"') {
		parser.Fail("Quoted parameter should start with double quotes");
	}

	res += params[index].substr(1);
	index++;

	while (index < params.size()) {
		res += " " + params[index];
		index++;

		if (res.back() == '"') {
			res.pop_back();
			break;
		}
	}

	return res;
}

RequireResult SQLLogicTestRunner::CheckRequire(SQLLogicParser &parser, const vector<string> &params) {
	if (params.size() < 1) {
		parser.Fail("require requires a single parameter");
	}
	// require command
	string param = StringUtil::Lower(params[0]);
	// os specific stuff

	if (param == "notmusl") {
#ifdef __MUSL_ENABLED__
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}
	if (param == "notmingw") {
#ifdef __MINGW32__
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "mingw") {
#ifndef __MINGW32__
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "notwindows") {
#ifdef _WIN32
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "windows") {
#ifndef _WIN32
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "linux") {
#ifndef __linux__
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "longdouble") {
#if LDBL_MANT_DIG < 54
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "64bit") {
		if (sizeof(void *) != 8) {
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "noforcestorage" || param == "no_force_storage") {
		if (TestConfiguration::TestForceStorage()) {
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "nothreadsan") {
#ifdef DUCKDB_THREAD_SANITIZER
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "strinline") {
#ifdef DUCKDB_DEBUG_NO_INLINE
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "ram") {
		if (params.size() != 2) {
			parser.Fail("require ram requires a parameter");
		}
		// require a minimum amount of ram
		auto required_limit = DBConfig::ParseMemoryLimitOrMaximum(params[1]);
		auto limit = FileSystem::GetAvailableMemory();
		if (!limit.IsValid()) {
			return RequireResult::MISSING;
		}
		if (limit.GetIndex() < required_limit) {
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "disk_space") {
		if (params.size() != 2) {
			parser.Fail("require disk_space requires a parameter");
		}
		// require a minimum amount of disk space
		auto required_limit = DBConfig::ParseMemoryLimitOrMaximum(params[1]);
		auto available_space = FileSystem::GetAvailableDiskSpace(".");
		if (!available_space.IsValid()) {
			return RequireResult::MISSING;
		}
		if (available_space.GetIndex() < required_limit) {
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "vector_size") {
		if (params.size() != 2) {
			parser.Fail("require vector_size requires a parameter");
		}
		// require a specific vector size
		auto required_vector_size = NumericCast<idx_t>(std::stoi(params[1]));
		if (STANDARD_VECTOR_SIZE < required_vector_size) {
			// vector size is too low for this test: skip it
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "exact_vector_size") {
		if (params.size() != 2) {
			parser.Fail("require exact_vector_size requires a parameter");
		}
		// require an exact vector size
		auto required_vector_size = NumericCast<idx_t>(std::stoi(params[1]));
		if (STANDARD_VECTOR_SIZE != required_vector_size) {
			// vector size does not match the required vector size: skip it
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "block_size") {
		if (params.size() != 2) {
			parser.Fail("require block_size requires a parameter");
		}
		// require a specific block size
		auto required_block_size = NumericCast<idx_t>(std::stoi(params[1]));
		auto block_size = Settings::Get<DefaultBlockSizeSetting>(*config);
		if (block_size != required_block_size) {
			// block size does not match the required block size: skip it
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}

	if (param == "skip_reload") {
		skip_reload = true;
		return RequireResult::PRESENT;
	}

	if (param == "no_alternative_verify") {
#ifdef DUCKDB_ALTERNATIVE_VERIFY
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "no_latest_storage") {
#ifdef DUCKDB_LATEST_STORAGE
		return RequireResult::MISSING;
#elif defined(DUCKDB_ALTERNATIVE_VERIFY)
		//! ALTERNATIVE_VERIFY also forces latest storage
		return RequireResult::MISSING;
#else
		return RequireResult::PRESENT;
#endif
	}

	if (param == "no_vector_verification") {
		auto &test_config = TestConfiguration::Get();
		if (test_config.GetVectorVerification() != DebugVectorVerification::NONE) {
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}
	if (param == "no_extension_autoloading") {
		if (params.size() < 2) {
			parser.Fail("require no_extension_autoloading needs an explanation string");
		}
		size_t index = 1;
		string explanation = ParseExplanation(parser, params, index);
		if (explanation.rfind("EXPECTED", 0) == 0 || explanation.rfind("FIXME", 0) == 0) {
			// good, explanation is properly formatted
		} else {
			parser.Fail(
			    "require no_extension_autoloading explanation string should begin with either 'EXPECTED' or FIXME'");
		}
		if (Settings::Get<AutoloadKnownExtensionsSetting>(*config)) {
			// If autoloading is on, we skip this test
			return RequireResult::MISSING;
		}
		return RequireResult::PRESENT;
	}
	if (param == "allow_unsigned_extensions") {
		if (Settings::Get<AllowUnsignedExtensionsSetting>(*config)) {
			return RequireResult::PRESENT;
		}
		return RequireResult::MISSING;
	}
	if (param == "vacuum_rebuild_indexes") {
		if (Settings::Get<VacuumRebuildIndexesSetting>(*config) > 0) {
			return RequireResult::PRESENT;
		}
		return RequireResult::MISSING;
	}

	bool excluded_from_autoloading = true;
	for (const auto &ext : AUTOLOADABLE_EXTENSIONS) {
		if (ext == param) {
			excluded_from_autoloading = false;
			break;
		}
	}

	bool perform_install = false;
	bool perform_load = false;
	if (!Settings::Get<AutoloadKnownExtensionsSetting>(*config)) {
		auto result = ExtensionLoadResult::NOT_LOADED;
		try {
			result = SQLLogicTestRunner::LoadExtension(*db, param);
		} catch (std::exception &ex) {
			ErrorData error_data(ex);
			parser.Fail("extension '%s' load threw an exception: %s", param, error_data.Message());
		}

		if (result == ExtensionLoadResult::LOADED_EXTENSION) {
			// add the extension to the list of loaded extensions
			extensions.insert(param);
		} else if (result == ExtensionLoadResult::EXTENSION_UNKNOWN) {
			parser.Fail("unknown extension type: %s", params[0]);
		} else if (result == ExtensionLoadResult::NOT_LOADED) {
			// extension known but not build: skip this test
			return RequireResult::MISSING;
		}
	} else if (excluded_from_autoloading) {
		if (autoloading_mode == TestConfiguration::ExtensionAutoLoadingMode::NONE) {
			// This is needed to still support LOCAL_EXTENSION_REPO
			return RequireResult::MISSING;
		}
		perform_install = true;
		perform_load = true;
	} else if (autoloading_mode != TestConfiguration::ExtensionAutoLoadingMode::NONE && autoinstall_is_checked) {
		perform_install = true;
	}
	if (perform_install) {
		auto res = con->Query("INSTALL " + param + " FROM '" + local_extension_repo + "';");
		if (res->HasError()) {
			return RequireResult::MISSING;
		}
	}
	if (perform_load) {
		auto res = con->Query("LOAD " + param + ";");
		if (res->HasError()) {
			return RequireResult::MISSING;
		}
		extensions.insert(param);
	}
	return RequireResult::PRESENT;
}

bool TryParseConditions(SQLLogicParser &parser, const string &condition_text, vector<Condition> &conditions,
                        bool skip_if) {
	bool is_condition = false;
	for (auto &c : condition_text) {
		switch (c) {
		case '=':
		case '>':
		case '<':
			is_condition = true;
			break;
		default:
			break;
		}
	}
	if (!is_condition) {
		// not a condition
		return false;
	}
	// split based on &&
	auto condition_strings = StringUtil::Split(condition_text, "&&");
	for (auto &condition_str : condition_strings) {
		vector<pair<string, ExpressionType>> comparators {
		    {"<>", ExpressionType::COMPARE_NOTEQUAL},   {">=", ExpressionType::COMPARE_GREATERTHANOREQUALTO},
		    {">", ExpressionType::COMPARE_GREATERTHAN}, {"<=", ExpressionType::COMPARE_LESSTHANOREQUALTO},
		    {"<", ExpressionType::COMPARE_LESSTHAN},    {"=", ExpressionType::COMPARE_EQUAL}};
		ExpressionType comparison_type = ExpressionType::INVALID;
		vector<string> splits;
		for (auto &comparator : comparators) {
			if (!StringUtil::Contains(condition_str, comparator.first)) {
				continue;
			}
			splits = StringUtil::Split(condition_str, comparator.first);
			comparison_type = comparator.second;
			break;
		}
		// loop condition, e.g. skipif threadid=0
		if (splits.size() != 2) {
			parser.Fail("skipif/onlyif must be in the form of x=y or x>y, potentially separated by &&");
		}
		// strip white space
		for (auto &split : splits) {
			StringUtil::Trim(split);
		}

		// now create the condition
		Condition condition;
		condition.keyword = splits[0];
		condition.value = splits[1];
		condition.comparison = comparison_type;
		condition.skip_if = skip_if;
		conditions.push_back(condition);
	}
	return true;
}

// add implicit tags from environment variables, with value if available
void add_env_tag(vector<string> &tags, const string &name, const string *value = nullptr) {
	tags.emplace_back(StringUtil::Format("env[%s]", name));
	if (value != nullptr) {
		tags.emplace_back(StringUtil::Format("env[%s]=%s", name, *value));
	}
}

void SQLLogicTestRunner::ConfigureDefaultInMemoryTemporaryDirectory(DuckDB &database, const string &db_path) {
	auto &db_config = DBConfig::GetConfig(*database.instance);
	if (!db_path.empty() || !db_config.options.use_temporary_directory ||
	    db_config.options.temporary_directory != ".tmp" || !Settings::Get<EnableExternalAccessSetting>(db_config)) {
		return;
	}
	auto normalized_script = StringUtil::Replace(file_name, "\\", "/");
	auto temp_directory_name = StringUtil::Replace(normalized_script, "/", "_");
	// an absolute Windows path has a drive colon, which a directory name cannot contain
	temp_directory_name = StringUtil::Replace(temp_directory_name, ":", "_");
	auto temp_directory = TestJoinPath(TestDirectoryPath(), "sqllogic_temp_" + temp_directory_name);
	db_config.SetOption(database.instance.get(), *DBConfig::GetOptionByName("temp_directory"), temp_directory);
}

void SQLLogicTestRunner::ExecuteFile(string script) {
	SQLLogicParser parser;
	bool success = parser.OpenFile(script);
	if (!success) {
		TEST_FAIL("Could not find test script '" + script + "'. Perhaps run `make sqlite`. ");
	}
	ExecuteInternal(parser, script);
}

void SQLLogicTestRunner::ExecuteStream(std::istream &input, const string &source_name) {
	SQLLogicParser parser;
	bool success = parser.OpenStream(input, source_name);
	if (!success) {
		TEST_FAIL("Could not read sqllogictest stream '" + source_name + "'");
	}
	ExecuteInternal(parser, source_name);
}

void SQLLogicTestRunner::ExecuteInternal(SQLLogicParser &parser, const string &script) {
	file_name = script;
	auto &test_config = TestConfiguration::Get();
	if (test_config.ShouldSkipTest(script)) {
		SkipTest("config skip_tests", TestSkipKind::EXCLUDED);
		return;
	}

	// for the original SQLite tests we convert floating point numbers to integers
	// for our own tests this is undesirable since it hides certain errors
	if (script.find("test/sqlite/select") != string::npos) {
		original_sqlite_test = true;
	}
	if (script.find("third_party/sqllogictest") != string::npos) {
		original_sqlite_test = true;
	}

	if (!dbpath.empty()) {
		// delete the target database file, if it exists
		DeleteDatabase(dbpath);
	}

	ignore_error_messages.clear();
	for (auto ignore : test_config.ErrorMessagesToBeSkipped()) {
		ignore_error_messages.insert(ignore);
	}

	// initialize the database with the default dbpath
	LoadDatabase(dbpath, true);

	auto init_sqllogic = test_config.GetInitSqllogic();
	if (!init_sqllogic.empty()) {
		SQLLogicParser init_parser;
		if (!init_parser.OpenFile(init_sqllogic)) {
			TEST_FAIL("Could not find init_sqllogic '" + init_sqllogic + "'");
		}
		ExecuteScript(init_parser, script);
	}

	EmitStatusEvent("execution-begin", script, "", "");
	ExecuteScript(parser, script);

	EmitExecutionComplete(script, test_skipped_requirement ? "skip-requirement" : "ok", test_skip_reason);
	auto cleanup_sqllogic = test_config.GetCleanupSqllogic();
	if (!cleanup_sqllogic.empty()) {
		SQLLogicParser cleanup_parser;
		if (!cleanup_parser.OpenFile(cleanup_sqllogic)) {
			TEST_FAIL("Could not find cleanup_sqllogic '" + cleanup_sqllogic + "'");
		}
		ExecuteScript(cleanup_parser, script);
	}
}

void SQLLogicTestRunner::ExecuteScript(SQLLogicParser &parser, const string &script) {
	auto &test_config = TestConfiguration::Get();
	idx_t skip_level = 0;
	bool test_expr_executed = false;
	bool file_tags_expr_seen = false;
	vector<string> file_tags; // gets both implicit and file-spec'd

	if (StringUtil::EndsWith(script, ".test_slow")) {
		file_tags.emplace_back("slow");
	}
	if (StringUtil::EndsWith(script, ".test_coverage")) {
		file_tags.emplace_back("coverage");
	}

	/* Loop over all records in the file */
	while (parser.NextStatement()) {
		// tokenize the current line
		auto token = parser.Tokenize();

		// throw explicit error on single line statements that are not separated by a comment or newline
		if (parser.IsSingleLineStatement(token) && !parser.NextLineEmptyOrComment()) {
			parser.Fail("all test statements need to be separated by an empty line");
		}

		// Check tags first time we hit test statements, since all explicit & implicit tags now present
		if (parser.IsTestCommand(token.type) && !test_expr_executed) {
			if (test_config.GetPolicyForTagSet(file_tags) == TestConfiguration::SelectPolicy::SKIP) {
				SkipTest("select tag-set", TestSkipKind::EXCLUDED);
				return;
			}
			test_expr_executed = true;
		}

		vector<Condition> conditions;
		bool skip_statement = false;
		while (token.type == SQLLogicTokenType::SQLLOGIC_SKIP_IF || token.type == SQLLogicTokenType::SQLLOGIC_ONLY_IF) {
			// skipif/onlyif
			bool skip_if = token.type == SQLLogicTokenType::SQLLOGIC_SKIP_IF;
			if (token.parameters.size() < 1) {
				parser.Fail("skipif/onlyif requires a single parameter (e.g. skipif duckdb)");
			}
			auto system_name = StringUtil::Lower(token.parameters[0]);
			// we support two kinds of conditions here
			// (for original sqllogictests) system comparisons, e.g.:
			// (1) skipif duckdb
			// (2) onlyif <other_system>
			// conditions on loop variables, e.g.:
			// (1) skipif i=2
			// (2) onlyif threadid=0
			// the latter is only supported in our own tests (not in original sqllogic tests)
			bool is_system_comparison;
			if (original_sqlite_test) {
				is_system_comparison = true;
			} else {
				is_system_comparison = !TryParseConditions(parser, system_name, conditions, skip_if);
			}
			if (is_system_comparison) {
				bool our_system = system_name == "duckdb";
				if (original_sqlite_test) {
					our_system = our_system || system_name == "postgresql";
				}
				if (our_system == skip_if) {
					// we skip this command in two situations
					// (1) skipif duckdb
					// (2) onlyif <other_system>
					skip_statement = true;
					break;
				}
			}
			parser.NextLine();
			token = parser.Tokenize();
		}
		if (skip_statement) {
			continue;
		}
		if (skip_level > 0 && token.type != SQLLogicTokenType::SQLLOGIC_MODE) {
			if (token.type == SQLLogicTokenType::SQLLOGIC_STATEMENT ||
			    token.type == SQLLogicTokenType::SQLLOGIC_QUERY) {
				CountSkipMode();
			}
			continue;
		}
		if (token.type == SQLLogicTokenType::SQLLOGIC_STATEMENT) {
			// statement
			if (token.parameters.size() < 1) {
				parser.Fail("statement requires at least one parameter (statement ok/error)");
			}
			auto command = make_uniq<Statement>(*this);

			bool original_output_result_mode = output_result_mode;

			// parse the first parameter
			if (token.parameters[0] == "ok") {
				command->expected_result = ExpectedResult::RESULT_SUCCESS;
			} else if (token.parameters[0] == "error") {
				command->expected_result = ExpectedResult::RESULT_ERROR;
			} else if (token.parameters[0] == "maybe") {
				command->expected_result = ExpectedResult::RESULT_UNKNOWN;
			} else if (token.parameters[0] == "debug") {
				command->expected_result = ExpectedResult::RESULT_DONT_CARE;
				output_result_mode = true;
			} else if (token.parameters[0] == "debug_skip") {
				command->expected_result = ExpectedResult::RESULT_DONT_CARE;
				output_result_mode = true;
				skip_level++;
			} else {
				parser.Fail("statement argument should be 'ok' or 'error");
			}

			command->file_name = script;
			command->query_line = parser.current_line + 1;

			// extract the SQL statement
			parser.NextLine();
			auto statement_text = parser.ExtractStatement();
			if (statement_text.empty()) {
				parser.Fail("Unexpected empty statement text");
			}
			command->expected_error = parser.ExtractExpectedError(command->expected_result, original_sqlite_test);

			// perform any renames in the text
			command->base_sql_query = ReplaceKeywords(std::move(statement_text));

			if (token.parameters.size() >= 2) {
				command->connection_name = token.parameters[1];
			}
			command->conditions = std::move(conditions);
			ExecuteCommand(std::move(command));
			output_result_mode = original_output_result_mode;
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_QUERY) {
			if (token.parameters.size() < 1) {
				parser.Fail("query requires at least one parameter (query III)");
			}
			auto command = make_uniq<Query>(*this);

			// parse the expected column count
			command->expected_column_count = 0;
			auto &column_text = token.parameters[0];
			for (idx_t i = 0; i < column_text.size(); i++) {
				command->expected_column_count++;
				if (column_text[i] != 'T' && column_text[i] != 'I' && column_text[i] != 'R') {
					parser.Fail("unknown type character '%s' in string, expected T, I or R only",
					            string(1, column_text[i]));
				}
			}
			if (command->expected_column_count == 0) {
				parser.Fail("query requires at least a single column in the result");
			}

			command->file_name = script;
			command->query_line = parser.current_line + 1;

			// extract the SQL statement
			parser.NextLine();
			auto statement_text = parser.ExtractStatement();

			// perform any renames in the text
			command->base_sql_query = ReplaceKeywords(std::move(statement_text));

			// extract the expected result
			command->values = parser.ExtractExpectedResult();

			// figure out the sort style/connection style
			string sort_style = "none";
			if (token.parameters.size() > 1) {
				if (!TestConfiguration::TryParseSortStyle(token.parameters[1], command->sort_style)) {
					// if this is not a known sort style, we use this as the connection name
					// this is a bit dirty, but well
					command->connection_name = token.parameters[1];
				} else {
					sort_style = token.parameters[1];
				}
			}
			if (!TestConfiguration::TryParseSortStyle(sort_style, command->sort_style)) {
				throw std::runtime_error("eek invalid sort style set, this should not happen");
			}

			// check the label of the query
			if (token.parameters.size() > 2) {
				command->query_has_label = true;
				command->query_label = token.parameters[2];
			} else {
				command->query_has_label = false;
			}
			command->conditions = std::move(conditions);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_HASH_THRESHOLD) {
			if (token.parameters.size() != 1) {
				parser.Fail("hash-threshold requires a parameter");
			}
			try {
				hash_threshold = std::stoi(token.parameters[0]);
			} catch (...) {
				parser.Fail("hash-threshold must be a number");
			}
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_HALT) {
			break;
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_MODE) {
			if (token.parameters.empty()) {
				parser.Fail("mode requires at least one parameter");
			}
			string parameter = token.parameters[0];
			if (parameter == "skip") {
				string reason = "unspecified";
				if (token.parameters.size() > 1) {
					reason = token.parameters[1];
					for (idx_t i = 2; i < token.parameters.size(); i++) {
						reason += " " + token.parameters[i];
					}
				}
				auto skip_reason = "mode skip " + reason;
				AddSkipReason(skip_reason);
				partial_skip_reasons.insert(std::move(skip_reason));
				skip_level++;
			} else if (parameter == "unskip") {
				skip_level--;
			} else {
				auto command = make_uniq<ModeCommand>(*this, std::move(parameter));
				ExecuteCommand(std::move(command));
			}
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_SET) {
			if (token.parameters.size() < 1) {
				parser.Fail("set requires at least 1 parameter (e.g. set ignore_error_messages HTTP Error)");
			}
			if (token.parameters[0] == "ignore_error_messages" || token.parameters[0] == "always_fail_error_messages") {
				unordered_set<string> *string_set;
				if (token.parameters[0] == "ignore_error_messages") {
					string_set = &ignore_error_messages;
				} else {
					string_set = &always_fail_error_messages;
				}

				// the set command overrides the default values
				string_set->clear();

				// Parse the parameter list as a comma separated list of strings that can contain spaces
				// e.g. `set ignore_error_messages This is an error message, This_is_another, and   another`
				if (token.parameters.size() > 1) {
					string current_string = "";
					unsigned int token_idx = 1;
					unsigned int substr_idx = 0;
					while (token_idx < token.parameters.size()) {
						auto comma_pos = token.parameters[token_idx].find(',', substr_idx);
						if (comma_pos == string::npos) {
							current_string += token.parameters[token_idx].substr(substr_idx) + " ";
							token_idx++;
							substr_idx = 0;
						} else {
							current_string += token.parameters[token_idx].substr(substr_idx, comma_pos);
							StringUtil::Trim(current_string);
							string_set->insert(current_string);
							current_string = "";
							substr_idx = comma_pos + 1;
						}
					}
					StringUtil::Trim(current_string);
					string_set->insert(current_string);
					string_set->erase("");
				}
			} else if (token.parameters[0] == "seed") {
				if (token.parameters.size() != 2) {
					parser.Fail("set seed requires a single seed value");
				}
				auto seed = Value(token.parameters[1]).DefaultTryCastAs(LogicalType::DOUBLE);
				if (!seed) {
					parser.Fail("set seed requires a floating point parameter");
				}
				auto res = con->Query("SELECT SETSEED(" + seed->ToString() + ")");
				if (res->HasError()) {
					parser.Fail("Failed to set seed: %s", res->GetError());
				}
				skip_reload = true;
			} else if (token.parameters[0] == "variable") {
				if (token.parameters.size() != 3) {
					parser.Fail("set variable requires two parameters (name value)");
				}
				auto &var_name = token.parameters[1];
				auto var_value = token.parameters[2];
				if (IsVariableReplacement(var_value)) {
					string variable_name;
					auto val = GetVariableReplacement(var_value, variable_name);
					var_value = val.ToString();
				}
				environment_variables[var_name] = var_value;
			} else {
				parser.Fail("unrecognized set parameter: %s", token.parameters[0]);
			}
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_RESET) {
			if (token.parameters.size() != 2) {
				parser.Fail("Expected reset [type] [name] (e.g reset label my_label)");
			}
			auto &reset_type = token.parameters[0];
			auto &reset_item = token.parameters[1];
			if (StringUtil::CIEquals("label", reset_type)) {
				auto reset_label_command = make_uniq<ResetLabel>(*this);
				reset_label_command->query_label = reset_item;
				ExecuteCommand(std::move(reset_label_command));
			} else {
				parser.Fail("unrecognized reset parameter: %s", reset_type);
			}
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_LOOP ||
		           token.type == SQLLogicTokenType::SQLLOGIC_CONCURRENT_LOOP) {
			if (token.parameters.size() != 3) {
				parser.Fail("Expected loop [iterator_name] [start] [end] (e.g. loop i 1 300)");
			}
			LoopDefinition def;
			def.loop_iterator_name = token.parameters[0];
			try {
				def.loop_start = std::stoi(token.parameters[1].c_str());
				def.loop_end = std::stoi(token.parameters[2].c_str());
			} catch (...) {
				parser.Fail("loop_start and loop_end must be a number");
			}
			def.loop_idx = def.loop_start;
			def.is_parallel = token.type == SQLLogicTokenType::SQLLOGIC_CONCURRENT_LOOP;
			StartLoop(def);
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_FOREACH ||
		           token.type == SQLLogicTokenType::SQLLOGIC_CONCURRENT_FOREACH) {
			if (token.parameters.size() < 2) {
				parser.Fail("expected foreach [iterator_name] [m1] [m2] [etc...] (e.g. foreach type integer "
				            "smallint float)");
			}
			LoopDefinition def;
			def.loop_iterator_name = token.parameters[0];
			for (idx_t i = 1; i < token.parameters.size(); i++) {
				D_ASSERT(!token.parameters[i].empty());
				def.tokens.push_back(token.parameters[i]);
			}
			def.loop_idx = 0;
			def.loop_start = 0;
			def.loop_end = def.tokens.size();
			def.is_parallel = token.type == SQLLogicTokenType::SQLLOGIC_CONCURRENT_FOREACH;
			StartLoop(def);
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_ENDLOOP) {
			EndLoop();
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_REQUIRE) {
			auto require_result = CheckRequire(parser, token.parameters);
			if (require_result == RequireResult::MISSING) {
				auto &param = token.parameters[0];
				if (IsRequired(param)) {
					// This extension / setting was explicitly required
					parser.Fail(StringUtil::Format("require %s: FAILED", param));
				}
				SkipTest("require " + token.parameters[0]);
				return;
			}
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_TEST_ENV) {
			if (InLoop()) {
				parser.Fail("test-env cannot be called in a loop");
			}

			if (token.parameters.size() != 2) {
				parser.Fail("test-env requires 2 arguments: <env name> <default env val>");
			}
			auto env_var = token.parameters[0];
			auto env_actual = test_config.GetTestEnv(env_var, token.parameters[1]);
			environment_variables[env_var] = env_actual;
			add_env_tag(file_tags, env_var, &env_actual);

		} else if (token.type == SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV ||
		           token.type == SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV_NOT) {
			auto exclude_values = token.type == SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV_NOT;
			string directive = exclude_values ? "require-env-not" : "require-env";
			if (InLoop()) {
				parser.Fail("%s cannot be called in a loop", directive);
			}

			if (token.parameters.empty()) {
				parser.Fail(exclude_values ? "require-env-not requires <env name> [excluded value ...]"
				                           : "require-env requires <env name> [expected value ...]");
			}
			auto skip_reason = directive + " " + StringUtil::Join(token.parameters, " ");

			auto &test_config = TestConfiguration::Get();
			auto env_var = token.parameters[0];
			auto test_env_defined = test_config.HasTestEnv(env_var);
			auto env_passed_through = GetEnvPassthroughNames().count(env_var) != 0;
			string env_actual_value;
			const char *env_actual = nullptr;
			if (test_env_defined || env_passed_through) {
				env_actual_value = test_config.GetTestEnv(env_var, "");
				env_actual = env_actual_value.c_str();
			} else {
				env_actual = std::getenv(env_var.c_str());
			}
			string default_local_repo = TestBuildDirectory() + "/repository";
			if (env_actual == nullptr && env_var == "LOCAL_EXTENSION_REPO" &&
			    Settings::Get<AutoloadKnownExtensionsSetting>(*config)) {
				// Overriding LOCAL_EXTENSION_REPO here is a hacky
				// More proper solution is wrapping std::getenv in a duckdb::test_getenv, and having a way to inject env
				// variables
				env_actual = default_local_repo.c_str();
			}
			if (exclude_values && token.parameters.size() == 1) {
				if (env_actual != nullptr) {
					SkipTest(skip_reason);
					return;
				}
				// An absent variable has no value to register for substitution or tagging.
				continue;
			}
			if (env_actual == nullptr) {
				// Environment variable was not found, this test should not be run
				SkipTest(skip_reason);
				return;
			}

			if (token.parameters.size() > 1) {
				bool matches = false;
				for (idx_t i = 1; i < token.parameters.size(); i++) {
					if (token.parameters[i] == env_actual) {
						matches = true;
						break;
					}
				}
				if (matches == exclude_values) {
					SkipTest(skip_reason);
					return;
				}

				file_tags.emplace_back(StringUtil::Format("env[%s]=%s", env_var, env_actual));
			}

			if (!test_env_defined && !env_passed_through && environment_variables.count(env_var)) {
				parser.Fail(StringUtil::Format("Environment variable '%s' has already been defined", env_var));
			}
			environment_variables[env_var] = env_actual;
			add_env_tag(file_tags, env_var, token.parameters.size() > 1 ? &environment_variables[env_var] : nullptr);

		} else if (token.type == SQLLogicTokenType::SQLLOGIC_LOAD) {
			auto &test_config = TestConfiguration::Get();
			if (test_config.OnLoadCommand() == "skip") {
				SkipTest("config on_load skip", TestSkipKind::EXCLUDED);
				return;
			}
			bool is_read_only = false;
			if (token.parameters.size() > 1) {
				auto param = token.parameters[1];
				if (StringUtil::CIEquals("readonly", param)) {
					is_read_only = true;
				} else if (StringUtil::CIEquals("readwrite", param)) {
					is_read_only = false;
				} else {
					parser.Fail(StringUtil::Format(
					    "parameter to 'load' is invalid, received '%s', accepted options are 'readonly' and 'readwrite",
					    param));
				}
			}

			string version;
			if (token.parameters.size() > 2) {
				version = token.parameters[2];
			}

			string load_db_path;
			if (!token.parameters.empty()) {
				load_db_path = ReplaceKeywords(token.parameters[0]);
			} else {
				load_db_path = string();
			}
			auto initial_db = test_config.GetInitialDBPath();
			if (!initial_db.empty() && !is_read_only && (!db || db->instance->config.options.database_path.empty())) {
				load_db_path = initial_db;
			}

			auto command = make_uniq<LoadCommand>(*this, load_db_path, is_read_only, version);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_RESTART) {
			bool load_extensions = !(token.parameters.size() == 1 && token.parameters[0] == "no_extension_load");

			// restart the current database
			auto command = make_uniq<RestartCommand>(*this, load_extensions);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_RECONNECT) {
			auto command = make_uniq<ReconnectCommand>(*this);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_SLEEP) {
			if (token.parameters.size() != 2) {
				parser.Fail("sleep requires two parameter (e.g. sleep 1 second)");
			}
			// require a specific block size
			auto sleep_duration = std::stoull(token.parameters[0]);
			auto sleep_unit = SleepCommand::ParseUnit(token.parameters[1]);
			auto command = make_uniq<SleepCommand>(*this, sleep_duration, sleep_unit);
			command->conditions = std::move(conditions);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_UNZIP) {
			if (token.parameters.size() != 1 && token.parameters.size() != 2) {
				parser.Fail("unzip requires 1 argument: <path/to/file.db.gz> [optional: "
				            "<path/to/unzipped_file.db>, default: {TEST_DIR}/<file.db>]");
			}

			// set input path
			auto input_path = ReplaceKeywords(token.parameters[0]);

			// file name
			idx_t filename_start_pos = input_path.find_last_of("/") + 1;
			if (!StringUtil::EndsWith(input_path, CompressionExtensionFromType(FileCompressionType::GZIP))) {
				parser.Fail("unzip: input has not a GZIP extension");
			}
			string filename = input_path.substr(filename_start_pos, input_path.size() - filename_start_pos - 3);

			// extraction path
			string default_extraction_path = ReplaceKeywords("{TEST_DIR}/" + filename);
			string extraction_path =
			    (token.parameters.size() == 2) ? ReplaceKeywords(token.parameters[1]) : default_extraction_path;
			if (extraction_path == "NULL") {
				extraction_path = default_extraction_path;
			}

			auto command = make_uniq<UnzipCommand>(*this, input_path, extraction_path);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_TAGS) {
			// NOTE: tags-before-test-commands is the low bar right now
			// 1 better: all non-command lines precede command lines
			// Mo better: parse first, build entire context before execution; allows e.g.
			// - implicit tag scans of e.g. strings, vars, etc., like '{ENVVAR}', '{TEST_DIR}', 'ATTACH'
			// - faster subset runs
			// - tag match runs to generate lists
			if (test_expr_executed) {
				parser.Fail("tags expression must precede test commands");
			}
			if (file_tags_expr_seen) {
				parser.Fail("tags may be only specified once");
			}
			file_tags_expr_seen = true;
			if (token.parameters.empty()) {
				parser.Fail("tags requires >= 1 argument, e.g.: <tag1> [tag2 .. tagN]");
			}

			// extend file_tags for jit eval
			file_tags.insert(file_tags.begin(), token.parameters.begin(), token.parameters.end());
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_CONTINUE) {
			if (!InLoop()) {
				parser.Fail("continue cannot be called outside of a loop");
			}
			auto command = make_uniq<ContinueCommand>(*this);
			command->conditions = std::move(conditions);
			ExecuteCommand(std::move(command));
		} else if (token.type == SQLLogicTokenType::SQLLOGIC_INCLUDE) {
			if (token.parameters.size() != 1) {
				parser.Fail("Expected include <path>");
			}
			parser.IncludeFile(token.parameters[0]);
		}
	}
	if (InLoop()) {
		parser.Fail("Missing endloop!");
	}
}

} // namespace duckdb
