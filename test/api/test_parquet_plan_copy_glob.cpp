#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "test_helpers.hpp"

#include <atomic>

using namespace duckdb;

namespace {

static std::atomic<idx_t> glob_count {0};

//! Serves glob-count://<name> from a local directory and counts the globs it expands
class GlobCountFileSystem : public LocalFileSystem {
public:
	explicit GlobCountFileSystem(string local_dir_p) : local_dir(std::move(local_dir_p)) {
	}

	static const string &Prefix() {
		static const string prefix = "glob-count://";
		return prefix;
	}

	string MapPath(const string &path) const {
		if (!StringUtil::StartsWith(path, Prefix())) {
			return path;
		}
		return local_dir + "/" + path.substr(Prefix().size());
	}

	string GetName() const override {
		return "GlobCountFileSystem";
	}

	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, Prefix());
	}

	bool FileExists(const string &path, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::FileExists(MapPath(path), opener);
	}

	unique_ptr<MultiFileList> GlobFilesExtended(const string &path, const FileGlobInput &input,
	                                            optional_ptr<FileOpener> opener) override {
		if (!CanHandleFile(path)) {
			return LocalFileSystem::GlobFilesExtended(path, input, opener);
		}
		if (HasGlob(path)) {
			++glob_count;
		}
		auto local_files = LocalFileSystem::GlobFilesExtended(MapPath(path), input, opener)->GetAllFiles();
		vector<OpenFileInfo> result;
		for (auto &file : local_files) {
			result.emplace_back(Prefix() + file.path.substr(local_dir.size() + 1));
		}
		return make_uniq<SimpleMultiFileList>(std::move(result));
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::OpenFile(MapPath(path), flags, opener);
	}

private:
	const string local_dir;
};

string InlinedCTEQuery(const string &source, idx_t uses) {
	string result = "WITH t AS NOT MATERIALIZED (SELECT * FROM " + source + ") SELECT ";
	for (idx_t i = 0; i < uses; i++) {
		result += StringUtil::Format("%s(SELECT count(*) + %llu FROM t) AS c%llu", i == 0 ? "" : ", ", i, i);
	}
	return result;
}

} // namespace

TEST_CASE("Copying a plan does not expand a parquet glob again", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto local_dir = TestCreatePath("parquet_plan_copy_glob");
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "COPY (SELECT i AS a, i %% 4 AS p FROM range(200) t(i)) TO '%s' (FORMAT parquet, PARTITION_BY (p))",
	    local_dir)));
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(make_uniq<GlobCountFileSystem>(local_dir));

	auto source = "read_parquet('" + GlobCountFileSystem::Prefix() + "**/*.parquet')";
	glob_count = 0;
	REQUIRE_NO_FAIL(con.Query("EXPLAIN " + InlinedCTEQuery(source, 5)));
	REQUIRE(glob_count == 1);

	glob_count = 0;
	auto result = con.Query(InlinedCTEQuery(source, 5));
	REQUIRE(CHECK_COLUMN(result, 0, {200}));
	REQUIRE(CHECK_COLUMN(result, 4, {204}));
	REQUIRE(glob_count == 1);
}

TEST_CASE("A copied single-file parquet scan still answers count(*) from its metadata", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto file = TestCreatePath("parquet_plan_copy_count.parquet");
	REQUIRE_NO_FAIL(
	    con.Query(StringUtil::Format("COPY (SELECT i AS a FROM range(10000) t(i)) TO '%s' (FORMAT parquet)", file)));

	auto result = con.Query("EXPLAIN " + InlinedCTEQuery("read_parquet('" + file + "')", 2));
	REQUIRE_NO_FAIL(*result);
	auto plan = StringUtil::Lower(result->ToString());
	REQUIRE(StringUtil::Contains(plan, "column data scan"));
	REQUIRE(!StringUtil::Contains(plan, "read parquet"));
	REQUIRE(!StringUtil::Contains(plan, "read_parquet"));
}

TEST_CASE("A copy of a parquet scan can be copied again", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto local_dir = TestCreatePath("parquet_plan_copy_twice");
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "COPY (SELECT i AS a, i %% 4 AS p FROM range(200) t(i)) TO '%s' (FORMAT parquet, PARTITION_BY (p))",
	    local_dir)));

	con.BeginTransaction();
	auto plan = con.ExtractPlan("SELECT * FROM read_parquet('" + local_dir + "/**/*.parquet')");
	auto copy = plan->Copy(*con.context);
	REQUIRE_NOTHROW(copy->Copy(*con.context));
	con.Commit();
}

TEST_CASE("A parquet file rewritten by an earlier statement is read again", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);

	// without validation, nothing but the end of the statement drops the metadata of the first read
	REQUIRE_NO_FAIL(con.Query("SET validate_external_file_cache = 'NO_VALIDATION'"));
	auto file = TestCreatePath("parquet_plan_copy_rewritten.parquet");
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("COPY (SELECT 1 AS a) TO '%s' (FORMAT parquet)", file)));
	auto result = con.Query(StringUtil::Format("CREATE TABLE first_read AS SELECT * FROM read_parquet('%s'); "
	                                           "COPY (SELECT 'x' AS b, 2 AS c) TO '%s' (FORMAT parquet); "
	                                           "SELECT * FROM read_parquet('%s')",
	                                           file, file, file));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 2);
	REQUIRE(CHECK_COLUMN(result, 0, {"x"}));
	REQUIRE(CHECK_COLUMN(result, 1, {2}));
}
