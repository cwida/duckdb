//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// test_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef _MSC_VER
// these break enum.hpp otherwise
#undef DELETE
#undef DEFAULT
#undef EXISTS
#undef IN
// this breaks file_system.cpp otherwise
#undef CreateDirectory
#undef RemoveDirectory
#endif

#include "compare_result.hpp"
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/types.hpp"
#include "test_config.hpp"
#include <sstream>
#include <iostream>

namespace duckdb {

void RegisterSqllogictests();
void RegisterSqllogictests(const vector<string> &test_paths);
void RegisterSqllogictestStdin();
bool SummarizeFailures();
//! Test directories of the extensions this build registered with LOAD_TESTS
vector<string> LoadedExtensionTestPaths();
//! The build directory of this build
string TestBuildDirectory();
//! The root directory that test paths are relative to (UNITTEST_ROOT_DIRECTORY, or the source directory)
string TestRootDirectory();

//! Test identity: the full test name sanitized to one filesystem/shell-safe path component (every char
//! outside [A-Za-z0-9_-] -> '_', including '.'). The body suffix is kept, so siblings differing only by
//! suffix (foo.test vs foo.test_slow) stay distinct (foo_test vs foo_test_slow). Shared by the TEST_ID
//! env var and the temp-dir leaf so they agree.
string TestNameToId(const string &name);

//! Delete a database file (+ its .wal). A no-op under --database-destroy off (retain).
void DeleteDatabase(string path);
void TestDeleteDirectory(string path);
void TestCreateDirectory(string path);
string TestJoinPath(string path1, string path2);
//! `path` made absolute against `anchor`. Already-absolute inputs are returned unchanged, including
//! a scheme'd URI (az://...) -- absolute, but not local.
string TestMakeAbsolute(const string &path, const string &anchor);
void TestDeleteFile(string path);
void TestChangeDirectory(string path);

void SetDeleteTestPath(bool delete_path);
bool DeleteTestPath();
void ClearTestDirectory();
string TestGetCurrentDirectory();
string TestDirectoryPath();
string TestCreatePath(string suffix);

//! The HOME sandbox ("<run-root>-home"), a sibling of the run root and never inside any {TEST_DIR}.
//! main points HOME/USERPROFILE here once per invocation. Always local -- a scheme'd URI cannot back
//! ~/.duckdb, so a remote root resolves against the local tree.
string GetTempDirHome();

//! Extensions the driver has compiled into itself, installed into every database the runner
//! creates. The unittest binary links debug_fs and registers it here; the sqllogictest extension
//! links none, so tests needing one skip or fail rather than the runner depending on it.
void SetStaticExtensionLoader(std::function<void(DuckDB &)> loader);

void LoadStaticExtensions(DuckDB &db);

void SetEmitTestEvents(bool emit);
bool EmitTestEventsEnabled();

unique_ptr<DBConfig> GetTestConfig();
bool TestIsInternalError(unordered_set<string> &internal_error_messages, const string &error);

// -----------------------------------------------------------------------------
// --temp-dir-* family: TEMP_DIR = $ROOT / [$RUN_ID] / [$TEST_ID]
//
// Two independent, toggleable nesting levels, plus a destroy disposition
// inherited down every level.
//
//   $ROOT    -- root; default "duckdb_unittest_tempdir" or --temp-dir-root <p>
//   $RUN_ID  -- per-invocation isolation (a random UUID when auto, or a caller id)
//   $TEST_ID -- per-test isolation (full test name, sanitized); resolved at test-run time
//
//! Destroy disposition: what to do about the dir at end-of-invocation / end-of-test.
enum class TempDirDestroy : uint8_t {
	NEVER,      //! retain
	ON_SUCCESS, //! remove on pass, retain on fail (default)
	ALWAYS      //! remove regardless
};
//! --database-destroy: disposition of loaded database files (DeleteDatabase). Independent of the
//! temp-dir dispositions -- a DB's keep/destroy is not per-se a temp-dir property. OFF restores the
//! old --test-temp-dir behavior where DeleteDatabase was a no-op (so retained temp dirs keep their
//! DB files). ON_SUCCESS mirrors --temp-dir-destroy so a failed test's DB survives for inspection.
enum class DatabaseDestroy : uint8_t {
	ON,        //! always delete DB files
	OFF,       //! never delete -- retain DB files
	ON_SUCCESS //! delete on pass, retain on fail (default)
};

//! Overrides the default root ("duckdb_unittest_tempdir"). Env TEMP_DIR_ROOT.
void SetTempDirRoot(const string &root);
//! Returns the resolved root ("duckdb_unittest_tempdir" by default).
string GetTempDirRoot();
//! Root for the LOCAL_TEMP_DIR tree when the primary root is remote -- pins local output somewhere
//! chosen instead of the default local root. Must itself be local, and is an error against a
//! differing local primary, which is already its own LOCAL_TEMP_DIR.
void SetLocalTempDirRoot(const string &root);
//! The --local-temp-dir-root override, or "" when unset (the default local root is used).
string GetLocalTempDirRoot();
//! Returns the resolved RUN_ID value ("" when RUN_ID is off).
string GetTempDirRunId();
//! --temp-dir-run-id {off|auto|<id>}: off => no RUN_ID level; auto => generated
//! UUID (once per invocation); any other value is used literally as the RUN_ID.
void SetRunId(const string &id);
bool SetTempDirRunIdInPath(const string &mode);
//! --temp-dir-test-id {on|off}. Returns false on an unrecognized value.
bool SetTempDirTestId(const string &mode);
//! String setter used by the CLI; returns false on an unrecognized value.
bool SetTempDirDestroy(const string &mode);
//! --database-destroy {on|off|on-success}. Returns false on an unrecognized value.
bool SetDatabaseDestroy(const string &mode);
//! Whether the post-test DB cleanup should delete, given this test's pass/fail (on-success aware).
bool DatabaseDestroyFires(bool success);
//! Materializes $ROOT + $RUN_ID once, at startup (main). Fills error + returns false on failure.
bool PrepareTempDir(string &error);
//! Reclaim the owned leaf and any empty ancestors created by the same lifecycle step.
void ReclaimTestDirectoryLevels(FileSystem &fs, const string &leaf, const vector<string> &created);
//! Executes the run-id-level destroy disposition (recursive, bottom-up "destroy only what I created").
void DestroyTempDir(bool success);
//! Executes the TEST_ID destroy disposition using THIS test's pass/fail (called at test end).
void DestroyTestTempDir(bool success);
//! LOCAL_TEMP_DIR: the guaranteed-local temp dir -- same composition, lifecycle and per-test
//! $TEST_ID level as TestDirectoryPath(). It IS TestDirectoryPath() when the root is local; a
//! separate local tree rooted at the default local root when remote.
string LocalTestDirectoryPath();
// -----------------------------------------------------------------------------

void SetEmitOnSkip(bool emit);
bool EmitOnSkipEnabled();
void SetDebugInitialize(int value);
void AddRequire(string require);
bool IsRequired(string require);

// -----------------------------------------------------------------------------
// --env-passthrough NAME (repeatable; also DUCKDB_TEST_ENV_PASSTHROUGH / config `env_passthrough`,
// comma-separated): pre-registers an env var for {NAME} substitution in every test, the treatment
// DATA_DIR/TEMP_DIR get, so no require-env/test-env is needed in the body. Unlike require-env's
// per-test skip, a named-but-absent var fails the whole invocation at startup.
//! True for a name the runner resolves for itself (TEMP_DIR, DATA_DIR, LOCAL_*, RUN_ID, the
//! {KEYWORD} substitutions, ...). These are reserved against EVERY external input -- neither
//! --env-passthrough nor a config `test_env` entry may set one: the runner computes them, once
//! per invocation or once per test, and an override would silently shadow the resolved value.
bool IsReservedEnvName(const string &name);
void AddEnvPassthrough(string name);
const duckdb::set<string> &GetEnvPassthroughNames();
//! false + `error` set if a --env-passthrough name is reserved by the runner, or absent from the env.
bool ValidateEnvPassthrough(string &error);
// -----------------------------------------------------------------------------

bool NO_FAIL(QueryResult &result);
bool NO_FAIL(duckdb::unique_ptr<QueryResult> result);

//! Submit a query and open a stream on its handle
duckdb::unique_ptr<duckdb::QueryResultStream<>> OpenStream(duckdb::Connection &con, const string &query);
//! Drain a stream into a retained result, so it can be checked with CHECK_COLUMN
duckdb::unique_ptr<duckdb::QueryResult> DrainStream(duckdb::QueryResultStream<> &stream);

#define REQUIRE_NO_FAIL(result) REQUIRE(NO_FAIL((result)))
#define REQUIRE_FAIL(result)    REQUIRE((result)->HasError())

#define COMPARE_CSV(result, csv, header)                                                                               \
	{                                                                                                                  \
		auto res = compare_csv(*result, csv, header);                                                                  \
		if (!res.empty())                                                                                              \
			FAIL(res);                                                                                                 \
	}

#define COMPARE_CSV_COLLECTION(collection, csv, header)                                                                \
	{                                                                                                                  \
		auto res = compare_csv_collection(collection, csv, header);                                                    \
		if (!res.empty())                                                                                              \
			FAIL(res);                                                                                                 \
	}

} // namespace duckdb
