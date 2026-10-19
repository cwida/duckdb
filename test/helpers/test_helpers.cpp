// #define CATCH_CONFIG_RUNNER
#include "test_reporter.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "compare_result.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/execution/operator/csv_scanner/string_value_scanner.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "test_config.hpp"
#include "pid.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/common/types/uuid.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TESTING_DIRECTORY_NAME "duckdb_unittest_tempdir"

namespace duckdb {
static case_insensitive_set_t required_requires;
static bool delete_test_path = true;
static bool emit_on_skip = false; // --emit-on-skip opt-in: emit a [SKIP_TEST] marker per skipped test

// --temp-dir-* family state: TEMP_DIR = $ROOT/[RUN_ID]/[TEST_ID]
static bool temp_dir_run_id_in_path = true; // --temp-dir-run-id {on|off}: RUN_ID as a path level
static string temp_dir_run_id;              // RUN_ID value (--run-id, or generated when absent)
static bool temp_dir_test_id = true;
static TempDirDestroy temp_dir_destroy = TempDirDestroy::ON_SUCCESS;
// --database-destroy: independent of the temp-dir dispositions (see test_helpers.hpp).
static DatabaseDestroy database_destroy = DatabaseDestroy::ON_SUCCESS;

// The primary (--temp-dir-root, may be remote) and, when that is remote, the local tree behind
// LOCAL_TEMP_DIR. Paths, not strings: easily composed, and remoteness is a state instead of a parse.
static Path primary_root = Path::FromString(TESTING_DIRECTORY_NAME);
static Path local_root;                     // resolved lazily; see LocalRoot()
static bool local_root_resolved = false;    // Path() is ".", not empty, so track this separately
static string local_temp_dir_root_override; // --local-temp-dir-root ("" -> the default local root)

// Levels THIS invocation created, split by lifecycle step so each destroy reclaims only its own. One
// set, not one per root: a remote root is env-var passthrough only -- unittest holds no credentials, so
// it can never mkdir or reap one, and the local tree is the only thing with a lifecycle.
static vector<string> run_created_levels;  // $ROOT..$RUN_ID
static vector<string> home_created_levels; // the HOME sandbox sibling

//! The $TEST_ID dir a test materialized, and the levels its path created. Keyed by TEST_ID rather
//! than held in one global: test files can run concurrently, and every thread working on one test
//! (a concurrentloop spawns several) has to find that test's entry.
struct ActiveTestTempDir {
	string leaf;
	vector<string> created_levels;
};
static mutex active_test_lock;
static unordered_map<string, ActiveTestTempDir> active_tests;

bool NO_FAIL(QueryResult &result) {
	if (result.HasError()) {
		fprintf(stderr, "Query failed with message: %s\n", result.GetError().c_str());
	}
	return !result.HasError();
}

duckdb::unique_ptr<duckdb::QueryResultStream<>> OpenStream(duckdb::Connection &con, const string &query) {
	return duckdb::make_uniq<duckdb::QueryResultStream<>>(con.Submit(query));
}

duckdb::unique_ptr<duckdb::QueryResult> DrainStream(duckdb::QueryResultStream<> &stream) {
	auto statement_type = stream.GetStatementType();
	auto properties = stream.GetStatementProperties();
	auto names = stream.GetNames();
	auto client_properties = stream.GetClientProperties();
	auto collection =
	    duckdb::make_uniq<duckdb::ColumnDataCollection>(duckdb::Allocator::DefaultAllocator(), stream.GetTypes());
	duckdb::ColumnDataAppendState append_state;
	collection->InitializeAppend(append_state);
	while (auto chunk = stream.Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		collection->Append(append_state, *chunk);
	}
	if (stream.HasError()) {
		return duckdb::make_uniq<duckdb::QueryResult>(stream.GetErrorObject());
	}
	return duckdb::make_uniq<duckdb::QueryResult>(statement_type, properties, names, std::move(collection),
	                                              client_properties);
}

bool NO_FAIL(duckdb::unique_ptr<QueryResult> result) {
	return NO_FAIL(*result);
}

void TestDeleteDirectory(string path) {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	try {
		if (fs->DirectoryExists(path)) {
			fs->RemoveDirectory(path);
		}
	} catch (...) {
	}
}

void TestDeleteFile(string path) {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	try {
		fs->TryRemoveFile(path);
	} catch (...) {
	}
}

void TestChangeDirectory(string path) {
	// set the base path for the tests
	FileSystem::SetWorkingDirectory(path);
}

string TestGetCurrentDirectory() {
	return FileSystem::GetWorkingDirectory();
}

void DeleteDatabase(string path) {
	if (database_destroy == DatabaseDestroy::OFF) {
		return; // retain: never touch DB files
	}
	TestDeleteFile(path);
	TestDeleteFile(path + ".wal");
}

void TestCreateDirectory(string path) {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	fs->CreateDirectory(path);
}

string TestJoinPath(string path1, string path2) {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	return fs->JoinPath(path1, path2);
}

string TestMakeAbsolute(const string &path, const string &anchor) {
	// Path::IsAbsolute handles URI schemes correctly
	auto parsed = Path::FromString(path);
	if (parsed.IsAbsolute()) {
		return path;
	}
	return Path::FromString(anchor).Join(parsed).ToString();
}

void SetEmitOnSkip(bool emit) {
	emit_on_skip = emit;
}

bool EmitOnSkipEnabled() {
	return emit_on_skip;
}

void AddRequire(string require) {
	required_requires.insert(require);
}

bool IsRequired(string require) {
	return required_requires.count(require);
}

// -----------------------------------------------------------------------------
// --env-passthrough NAME: env vars pre-registered for {NAME} substitution -- see test_helpers.hpp.
// Case-SENSITIVE, matching both env var names and the test_env map these land in.
static duckdb::set<string> env_passthrough_names;

// Every name the runner resolves for itself -- passing one through would silently replace it with the
// ambient shell's. Covers both namespaces, because ReplaceKeywords substitutes env vars BEFORE
// ProcessPath, so a passthrough beats a {KEYWORD} too. Keep in sync with the three sites that own
// these: TestConfiguration::UpdateEnvironment, TestConfiguration::ProcessPath, RunSQLLogicTest.
static const char *const RESERVED_ENV_NAMES[] = {
    // test_env keys (UpdateEnvironment, plus RunSQLLogicTest's per-test overrides)
    "BUILD_DIR", "CATALOG_DIR", "DATA_DIR", "LOCAL_DATA_DIR", "LOCAL_TEMP_DIR", "RUN_ID", "TEMP_DIR",
    "TEMP_DIR_ABSOLUTE", "TEMP_DIR_ROOT", "TEST_ID", "TEST_NAME", "TEST_NAME__NO_SLASH", "TEST_UUID", "WORKING_DIR",
    // {KEYWORD} substitutions (ProcessPath, and ReplaceKeywords' own {BUILD_DIRECTORY})
    "BASE_TEST_NAME", "BUILD_DIRECTORY", "TEST_DIR", "UUID", "WORKING_DIRECTORY"};

bool IsReservedEnvName(const string &name) {
	for (auto &reserved : RESERVED_ENV_NAMES) {
		if (StringUtil::CIEquals(name, reserved)) {
			return true;
		}
	}
	return false;
}

void AddEnvPassthrough(string name) {
	env_passthrough_names.insert(name);
}

const duckdb::set<string> &GetEnvPassthroughNames() {
	return env_passthrough_names;
}

bool ValidateEnvPassthrough(string &error) {
	string missing;
	string reserved;
	for (auto &name : env_passthrough_names) {
		if (!std::getenv(name.c_str())) {
			missing += (missing.empty() ? "" : ", ") + name;
		}
		if (IsReservedEnvName(name)) {
			reserved += (reserved.empty() ? "" : ", ") + name;
		}
	}
	if (!reserved.empty()) {
		error = "--env-passthrough: reserved by the test runner, cannot be passed through: " + reserved;
		return false;
	}
	if (!missing.empty()) {
		error = "--env-passthrough: missing from the environment: " + missing;
		return false;
	}
	return true;
}
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --temp-dir-* family
//

static string ResolveRunId(); // always set, generated when --run-id is absent; defined below

void SetLocalTempDirRoot(const string &root) {
	if (root.empty()) {
		throw std::runtime_error("--local-temp-dir-root requires a non-empty value");
	}
	if (Path::FromString(root).IsRemote()) {
		throw std::runtime_error("--local-temp-dir-root must be a local path, got: " + root);
	}
	local_temp_dir_root_override = root;
}

string GetLocalTempDirRoot() {
	return local_temp_dir_root_override;
}

void SetTempDirRoot(const string &root) {
	// Check string: Path::FromString("") -> "." (path identity). Reject "" as invalid, but
	// don't confuse the acceptable ".".
	if (root.empty()) {
		throw std::runtime_error("--temp-dir-root requires a non-empty value");
	}
	primary_root = Path::FromString(root);
}

string GetTempDirRoot() {
	return primary_root.ToString();
}

string GetTempDirRunId() {
	return ResolveRunId();
}

void SetRunId(const string &id) {
	// Any value but "auto" is a caller-fixed id -- pytest passes one so a run's many unittest
	// batches co-locate under a single RUN_ID.
	temp_dir_run_id = (id == "auto") ? "" : id;
}

bool SetTempDirRunIdInPath(const string &mode) {
	if (mode == "on") {
		temp_dir_run_id_in_path = true;
	} else if (mode == "off") {
		temp_dir_run_id_in_path = false;
	} else {
		return false;
	}
	return true;
}

bool SetTempDirTestId(const string &mode) {
	if (mode == "on") {
		temp_dir_test_id = true;
	} else if (mode == "off") {
		temp_dir_test_id = false;
	} else {
		return false;
	}
	return true;
}

bool SetTempDirDestroy(const string &mode) {
	if (mode == "never") {
		temp_dir_destroy = TempDirDestroy::NEVER;
	} else if (mode == "on-success") {
		temp_dir_destroy = TempDirDestroy::ON_SUCCESS;
	} else if (mode == "always") {
		temp_dir_destroy = TempDirDestroy::ALWAYS;
	} else {
		return false;
	}
	return true;
}

bool SetDatabaseDestroy(const string &mode) {
	if (mode == "on") {
		database_destroy = DatabaseDestroy::ON;
	} else if (mode == "off") {
		database_destroy = DatabaseDestroy::OFF;
	} else if (mode == "on-success") {
		database_destroy = DatabaseDestroy::ON_SUCCESS;
	} else {
		return false;
	}
	return true;
}

bool DatabaseDestroyFires(bool success) {
	switch (database_destroy) {
	case DatabaseDestroy::ON:
		return true;
	case DatabaseDestroy::ON_SUCCESS:
		return success;
	case DatabaseDestroy::OFF:
	default:
		return false;
	}
}

static string GenerateAutoRunId() {
	// Keep the full UUID without timestamp or mnemonic overhead in Windows paths.
	return UUID::ToString(UUID::GenerateRandomUUID());
}

// The caller's --run-id, or a generated one, cached for the invocation. Always resolved, even when
// RUN_ID is not a path segment (TreeRunIdRoot gates that) -- the env var exists either way.
static string ResolveRunId() {
	if (temp_dir_run_id.empty()) {
		temp_dir_run_id = GenerateAutoRunId();
	}
	return temp_dir_run_id;
}

// $ROOT/[RUN_ID] -- stable across the invocation, no IO.
static Path TreeRunIdRoot(const Path &root) {
	if (!temp_dir_run_id_in_path) {
		return root;
	}
	return root.Join(ResolveRunId());
}

// The tree LOCAL_TEMP_DIR and HOME resolve against. A local primary IS the local tree, invariantly -- the
// two can never be different local dirs, and PrepareTempDir rejects a --local-temp-dir-root that would
// make them so. Resolved lazily: the local tree must be absolute (it outlives any test chdir) and the cwd
// is only final once main() has changed into it.
static const Path &LocalRoot() {
	if (!primary_root.IsRemote()) {
		return primary_root;
	}
	if (!local_root_resolved) {
		const string &raw =
		    local_temp_dir_root_override.empty() ? string(TESTING_DIRECTORY_NAME) : local_temp_dir_root_override;
		local_root = Path::FromString(TestMakeAbsolute(raw, TestGetCurrentDirectory()));
		local_root_resolved = true;
	}
	return local_root;
}

// A SIBLING of the run-id root, never inside it: HOME within a {TEST_DIR} that a test whitelists via
// allowed_directories would make ~/.duckdb an allowed path and break permission tests (INSTALL-is-denied
// and friends). Resolved against the local tree -- a scheme'd URI cannot back ~/.duckdb.
string GetTempDirHome() {
	// A sibling suffix, not a child, so this is string concatenation -- Path composes children only.
	return TreeRunIdRoot(LocalRoot()).ToString() + "-home";
}

// The FULL test name as one filesystem- and shell-safe path component. Shared by the TEST_ID env var
// and the temp-dir leaf so they agree. The body suffix is kept (dropping it would collide foo.test with
// foo.test_slow onto one dir); everything unsafe is mapped, not just '/' and '.', because Catch2 case
// names are free-form prose and the leaf must survive unquoted in tests that shell out via system().
string TestNameToId(const string &name) {
	string id;
	id.reserve(name.size());
	for (char c : name) {
		bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		id += safe ? c : '_';
	}
	return id;
}

// TEST_ID for the currently-executing test ("" when TEST_ID off or no test is active,
// e.g. at startup -- the test name isn't known until a test runs).
static string ResolveTestId() {
	if (!temp_dir_test_id) {
		return "";
	}
	string name;
	try {
		name = TestReporter::Get().CurrentTestName();
	} catch (...) {
		name = "";
	}
	if (name.empty()) {
		return "";
	}
	return TestNameToId(name);
}

// Full resolved path $ROOT/[RUN_ID]/[TEST_ID] for `tree`. No IO.
static Path TreeTestPath(const Path &root) {
	return TreeRunIdRoot(root).Join(ResolveTestId());
}

static bool DirectoryIsEmpty(FileSystem &fs, const string &path) {
	bool empty = true;
	fs.ListFiles(path, [&](const string &, bool) { empty = false; });
	return empty;
}

// mkdir-p `path`, recording into `created` (outermost..leaf) the levels that did not already exist,
// so the matching destroy removes only what it made. The walk stops at the first pre-existing
// ancestor -- levels owned by an earlier lifecycle step or a prior run are never recorded.
static void RecordAndCreateLevels(FileSystem &fs, const string &path, vector<string> &created) {
	vector<string> to_create; // innermost first
	string p = path;
	while (!p.empty() && !fs.DirectoryExists(p)) {
		to_create.push_back(p);
		auto parent = StringUtil::GetFilePath(p);
		if (parent.empty() || parent == p) {
			break;
		}
		p = parent;
	}
	fs.CreateDirectoriesRecursive(path);
	std::reverse(to_create.begin(), to_create.end()); // outermost..leaf
	created = to_create;
}

// Remove `leaf` recursively iff this step created it, then walk `created` bottom-up removing each
// ancestor iff it is empty and this step created it. Stops at the first level that is neither.
void ReclaimTestDirectoryLevels(FileSystem &fs, const string &leaf, const vector<string> &created) {
	// The leaf obeys the same rule as its ancestors: reclaim only what THIS step created. RemoveDirectory
	// is always recursive, so a run that adopted a pre-existing dir -- a reused --run-id, or $ROOT itself
	// when --temp-dir-run-id is off -- would otherwise delete contents it does not own.
	if (!created.empty() && created.back() == leaf && fs.DirectoryExists(leaf)) {
		try {
			fs.RemoveDirectory(leaf); // recursive
		} catch (...) {
		}
	}
	for (idx_t idx = created.size(); idx-- > 0;) {
		const auto &level = created[idx];
		if (level == leaf) {
			continue; // already removed above
		}
		if (!fs.DirectoryExists(level)) {
			continue;
		}
		if (!DirectoryIsEmpty(fs, level)) {
			break; // shared with someone else's content -> stop
		}
		try {
			// Another run may populate this ancestor after the emptiness check.
			if (!fs.RemoveDirectoryExtended(level, {RemoveDirectoryMode::SINGLE})) {
				break;
			}
		} catch (...) {
			break;
		}
	}
}

static bool DestroyFires(TempDirDestroy disposition, bool success) {
	switch (disposition) {
	case TempDirDestroy::ON_SUCCESS:
		return success;
	case TempDirDestroy::ALWAYS:
		return true;
	case TempDirDestroy::NEVER:
	default:
		return false;
	}
}

// mkdir-p the local tree's $ROOT/[RUN_ID], recording what it created so destroy reclaims only that.
static void PrepareLocalTree() {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	RecordAndCreateLevels(*fs, TreeRunIdRoot(LocalRoot()).ToString(), run_created_levels);
}

static bool PrepareTempDirInternal(string &error) {
	// Checked here, not at set time: it depends on two options, which arrive in any order from the CLI,
	// the environment and a config file.
	// Naming the same local root twice is redundant, not a conflict; only a divergent one is an error.
	if (!GetLocalTempDirRoot().empty() && !primary_root.IsRemote() &&
	    Path::Normalize(GetLocalTempDirRoot()) != primary_root.ToString()) {
		error = "--local-temp-dir-root conflicts with a local --temp-dir-root, which is already its own "
		        "LOCAL_TEMP_DIR; it applies only to a remote root. To relocate the whole tree, set "
		        "--temp-dir-root / DUCKDB_TEST_TEMP_DIR_ROOT instead (temp-dir-root: " +
		        primary_root.ToString() + ", local-temp-dir-root: " + GetLocalTempDirRoot() + ")";
		return false;
	}
	PrepareLocalTree();
	// LAST, and this ordering is load-bearing: HOME is a sibling of the run root, so creating it earlier
	// would materialize $ROOT behind PrepareLocalTree's back, RecordAndCreateLevels would read it as
	// pre-existing, and it would never be reclaimed. Unconditional -- some code errors if
	// home_directory is set but absent.
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	RecordAndCreateLevels(*fs, GetTempDirHome(), home_created_levels);
	return true;
}

bool PrepareTempDir(string &error) {
	// Every mkdir/rmdir above is a syscall that can fail for reasons the caller can do nothing about
	// (permissions, ENOSPC, a race). Funnel them into `error` -- this function's whole contract is to
	// report startup failures, and an escaping exception would terminate instead.
	try {
		return PrepareTempDirInternal(error);
	} catch (std::exception &ex) {
		error = ErrorData(ex).Message();
		return false;
	}
}

void DestroyTempDir(bool success) {
	if (!DestroyFires(temp_dir_destroy, success)) {
		return;
	}
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	// HOME first: it sits under $ROOT, so leaving it would fail the emptiness check in the ancestor
	// walk below and strand a root this run owns. Reclaimed on the same created/adopted rule --
	// co-located batches share one HOME path, and an adopted one may be another batch's live sandbox.
	ReclaimTestDirectoryLevels(*fs, GetTempDirHome(), home_created_levels);
	// The root subsumes any leftover per-test dirs; ancestors go only up to a pre-existing $ROOT.
	ReclaimTestDirectoryLevels(*fs, TreeRunIdRoot(LocalRoot()).ToString(), run_created_levels);
}

// Fired at test end with THIS test's pass/fail. Only the $TEST_ID level is in scope -- $RUN_ID/$ROOT
// were made by PrepareTempDir, so ReclaimTestDirectoryLevels stops there.
void DestroyTestTempDir(bool success) {
	ActiveTestTempDir entry;
	{
		lock_guard<mutex> guard(active_test_lock);
		auto it = active_tests.find(ResolveTestId());
		if (it == active_tests.end()) {
			return;
		}
		entry = std::move(it->second);
		active_tests.erase(it);
	}
	if (!entry.leaf.empty() && DestroyFires(temp_dir_destroy, success)) {
		duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
		ReclaimTestDirectoryLevels(*fs, entry.leaf, entry.created_levels);
	}
}

// $ROOT/[RUN_ID]/[TEST_ID], materializing the $TEST_ID level on first request -- lazily, because the
// test name isn't known until a test runs. Records only what this per-test path creates.
static string MaterializeLocalTestPath() {
	string path = TreeTestPath(LocalRoot()).ToString();
	string root = TreeRunIdRoot(LocalRoot()).ToString();
	if (path == root) {
		return path;
	}
	lock_guard<mutex> guard(active_test_lock);
	auto test_id = ResolveTestId();
	auto entry = active_tests.find(test_id);
	if (entry != active_tests.end() && entry->second.leaf == path) {
		return path;
	}
	ActiveTestTempDir active;
	active.leaf = path;
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	if (!fs->DirectoryExists(path)) {
		RecordAndCreateLevels(*fs, path, active.created_levels);
	}
	active_tests[test_id] = std::move(active);
	return path;
}
// -----------------------------------------------------------------------------

string TestDirectoryPath() {
	// A remote root is a pure string -- the test owns materialization there. Otherwise it IS the local tree.
	if (primary_root.IsRemote()) {
		return TreeTestPath(primary_root).ToString();
	}
	return MaterializeLocalTestPath();
}

string LocalTestDirectoryPath() {
	return MaterializeLocalTestPath();
}

void SetDeleteTestPath(bool delete_path) {
	delete_test_path = delete_path;
}

bool DeleteTestPath() {
	return delete_test_path;
}

static bool emit_test_events = false;

static std::function<void(DuckDB &)> static_extension_loader;

void SetStaticExtensionLoader(std::function<void(DuckDB &)> loader) {
	static_extension_loader = std::move(loader);
}

void LoadStaticExtensions(DuckDB &db) {
	if (static_extension_loader) {
		static_extension_loader(db);
	}
}

void SetEmitTestEvents(bool emit) {
	emit_test_events = emit;
}

bool EmitTestEventsEnabled() {
	return emit_test_events;
}

void ClearTestDirectory() {
	if (!DeleteTestPath()) {
		return;
	}
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	auto test_dir = TestDirectoryPath();
	// try to clear any files we created in the test directory
	fs->ListFiles(test_dir, [&](const string &file, bool is_dir) {
		auto full_path = fs->JoinPath(test_dir, file);
		try {
			if (is_dir) {
				fs->RemoveDirectory(full_path);
			} else {
				fs->RemoveFile(full_path);
			}
		} catch (...) {
			// skip
		}
	});
}

string TestCreatePath(string suffix) {
	return TestJoinPath(TestDirectoryPath(), suffix);
}

#ifndef _WIN32
bool TestOpenWithFailingClose(const string &path, const std::function<void()> &open_file) {
	// the next opened descriptor is the lowest free one
	auto fd = open("/dev/null", O_RDONLY);
	if (fd == -1 || close(fd) != 0) {
		return false;
	}
	open_file();
	struct stat fd_stat;
	struct stat path_stat;
	if (fstat(fd, &fd_stat) != 0 || stat(path.c_str(), &path_stat) != 0) {
		return false;
	}
	if (fd_stat.st_dev != path_stat.st_dev || fd_stat.st_ino != path_stat.st_ino) {
		return false;
	}
	return close(fd) == 0;
}
#endif

bool TestIsInternalError(unordered_set<string> &internal_error_messages, const string &error) {
	for (auto &error_message : internal_error_messages) {
		if (StringUtil::Contains(error, error_message)) {
			return true;
		}
	}
	return false;
}

unique_ptr<DBConfig> GetTestConfig() {
	auto &test_config = TestConfiguration::Get();

	auto result = make_uniq<DBConfig>();
#ifndef DUCKDB_ALTERNATIVE_VERIFY
	result->options.checkpoint_wal_size = test_config.GetCheckpointWALSize();
	result->options.checkpoint_on_shutdown = test_config.GetCheckpointOnShutdown();
#else
	result->options.checkpoint_on_shutdown = false;
#endif
	result->options.abort_on_wal_failure = true;
#ifdef DUCKDB_RUN_SLOW_VERIFIERS
	// This mode isn't slow, but we want test coverage both when it's enabled
	// and when it's not, so we enable only when DUCKDB_RUN_SLOW_VERIFIERS is set.
	result->options.trim_free_blocks = true;
#endif
	result->SetOptionByName("allow_unsigned_extensions", true);
	auto storage_version = test_config.GetStorageVersion();
	if (!storage_version.empty()) {
		result->options.storage_compatibility = StorageCompatibility::FromString(storage_version);
	}

	auto max_threads = test_config.GetMaxThreads();
	if (max_threads.IsValid()) {
		result->options.maximum_threads = max_threads.GetIndex();
	}

	auto block_alloc_size = test_config.GetBlockAllocSize();
	if (block_alloc_size.IsValid()) {
		Storage::VerifyBlockAllocSize(block_alloc_size.GetIndex());
		result->SetOptionByName("default_block_size", Value::UBIGINT(block_alloc_size.GetIndex()));
	}

	result->options.debug_initialize = test_config.GetDebugInitialize();
	result->options.main_database_options = test_config.GetMainDatabaseOptions();
	result->SetOptionByName("debug_verify_vector", EnumUtil::ToString(test_config.GetVectorVerification()));
	return result;
}

bool CHECK_COLUMN(QueryResult &result, size_t column_number, vector<duckdb::Value> values) {
	if (result.HasError()) {
		fprintf(stderr, "Query failed with message: %s\n", result.GetError().c_str());
		return false;
	}
	if (result.GetNames().size() != result.GetTypes().size()) {
		// column names do not match
		result.Print();
		return false;
	}
	if (values.empty()) {
		if (result.RowCount() != 0) {
			result.Print();
			return false;
		} else {
			return true;
		}
	}
	if (result.RowCount() == 0) {
		result.Print();
		return false;
	}
	if (column_number >= result.GetTypes().size()) {
		result.Print();
		return false;
	}
	auto rows = result.Collection().GetRows();
	for (idx_t row_idx = 0; row_idx < values.size(); row_idx++) {
		auto value = rows.GetValue(column_number, row_idx);
		// NULL <> NULL, hence special handling
		if (value.IsNull() && values[row_idx].IsNull()) {
			continue;
		}

		if (!Value::DefaultValuesAreEqual(value, values[row_idx])) {
			// FAIL("Incorrect result! Got " + vector.GetValue(j).ToString()
			// +
			//      " but expected " + values[i + j].ToString());
			result.Print();
			return false;
		}
	}
	return true;
}

bool CHECK_COLUMN(duckdb::unique_ptr<duckdb::QueryResult> &result, size_t column_number, vector<duckdb::Value> values) {
	return CHECK_COLUMN(*result, column_number, values);
}

string compare_csv(duckdb::QueryResult &result, string csv, bool header) {
	if (result.HasError()) {
		fprintf(stderr, "Query failed with message: %s\n", result.GetError().c_str());
		return result.GetError();
	}
	string error;
	if (!compare_result(csv, result.Collection(), result.GetTypes(), header, error)) {
		return error;
	}
	return "";
}

string compare_csv_collection(duckdb::ColumnDataCollection &collection, string csv, bool header) {
	string error;
	if (!compare_result(csv, collection, collection.Types(), header, error)) {
		return error;
	}
	return "";
}

string show_diff(DataChunk &left, DataChunk &right) {
	if (left.ColumnCount() != right.ColumnCount()) {
		return StringUtil::Format("Different column counts: %d vs %d", (int)left.ColumnCount(),
		                          (int)right.ColumnCount());
	}
	if (left.size() != right.size()) {
		return StringUtil::Format("Different sizes: %zu vs %zu", left.size(), right.size());
	}
	string difference;
	for (size_t i = 0; i < left.ColumnCount(); i++) {
		bool has_differences = false;
		auto &left_vector = left.data[i];
		auto &right_vector = right.data[i];
		string left_column = StringUtil::Format("Result\n------\n%s [", left_vector.GetType().ToString().c_str());
		string right_column = StringUtil::Format("Expect\n------\n%s [", right_vector.GetType().ToString().c_str());
		if (left_vector.GetType() == right_vector.GetType()) {
			for (size_t j = 0; j < left.size(); j++) {
				auto left_value = left_vector.GetValue(j);
				auto right_value = right_vector.GetValue(j);
				if (!Value::DefaultValuesAreEqual(left_value, right_value)) {
					left_column += left_value.ToString() + ",";
					right_column += right_value.ToString() + ",";
					has_differences = true;
				} else {
					left_column += "_,";
					right_column += "_,";
				}
			}
		} else {
			left_column += "...";
			right_column += "...";
		}
		left_column += "]\n";
		right_column += "]\n";
		if (has_differences) {
			difference += StringUtil::Format("Difference in column %d:\n", i);
			difference += left_column + "\n" + right_column + "\n";
		}
	}
	return difference;
}

//! Compares the result of a pipe-delimited CSV with the given DataChunk
//! Returns true if they are equal, and stores an error_message otherwise
bool compare_result(string csv, ColumnDataCollection &collection, vector<LogicalType> sql_types, bool has_header,
                    string &error_message) {
	D_ASSERT(collection.Count() == 0 || collection.Types().size() == sql_types.size());

	// create the csv on disk
	auto csv_path = TestCreatePath("__test_csv_path.csv");
	std::ofstream f(csv_path);
	f << csv;
	f.close();

	// set up the CSV reader
	CSVReaderOptions options;
	options.auto_detect = false;
	options.dialect_options.state_machine_options.delimiter = {"|"};
	options.dialect_options.header = has_header;
	options.dialect_options.state_machine_options.quote = '\"';
	options.dialect_options.state_machine_options.escape = '\"';
	options.file_path = csv_path;
	options.dialect_options.num_cols = sql_types.size();
	// set up the intermediate result chunk
	DataChunk parsed_result;
	parsed_result.Initialize(Allocator::DefaultAllocator(), sql_types);

	DuckDB db;
	Connection con(db);
	MultiFileOptions file_options;
	auto scanner_ptr = StringValueScanner::GetCSVScanner(*con.context, options, file_options);
	auto &scanner = *scanner_ptr;
	ColumnDataCollection csv_data_collection(*con.context, sql_types);
	while (!scanner.FinishedIterator()) {
		// parse a chunk from the CSV file
		try {
			parsed_result.Reset();
			scanner.Flush(parsed_result);
		} catch (std::exception &ex) {
			error_message = "Could not parse CSV: " + string(ex.what());
			return false;
		}
		if (parsed_result.size() == 0) {
			break;
		}
		csv_data_collection.Append(parsed_result);
	}
	string error;
	if (!ColumnDataCollection::ResultEquals(collection, csv_data_collection, error_message)) {
		return false;
	}
	return true;
}

} // namespace duckdb
