#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

//! A local catalog that reports itself as remote, so that attaching it enables the RemotePushdownOptimizer
class TestRemoteCatalog : public DuckCatalog {
public:
	explicit TestRemoteCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

public:
	bool Supports(RemoteCapability capability) const override {
		return capability == RemoteCapability::IS_REMOTE;
	}
};

struct TestRemoteStorageExtension : StorageExtension {
	TestRemoteStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, TestRemoteCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("Constant-folded macro arguments stay positional with a remote catalog attached", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "test_remote_test", make_shared_ptr<TestRemoteStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH 'test' AS s (TYPE test_remote_test)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE s.macro_args_tbl AS SELECT [1, 2, 3] AS arr"));

	// built-in macro with a foldable positional argument followed by another positional argument
	auto result = con.Query("SELECT generate_subscripts([1, 2], 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	result = con.Query("SELECT generate_subscripts([1, 2])");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	result = con.Query("SELECT generate_subscripts(arr := [1, 2], dim := 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	result = con.Query("SELECT generate_subscripts(NULL, 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {}));

	// folded argument that is itself a nested expression
	result = con.Query("SELECT generate_subscripts(list_value(1 + 1, 2 * 3, 4), 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));

	// user-defined macros
	REQUIRE_NO_FAIL(con.Query("CREATE MACRO add_default(a, b := 5) AS a + b"));
	result = con.Query("SELECT add_default(1 + 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
	result = con.Query("SELECT add_default(1 + 1, 2 * 3)");
	REQUIRE(CHECK_COLUMN(result, 0, {8}));
	result = con.Query("SELECT add_default(1 + 1, b := 2 * 3)");
	REQUIRE(CHECK_COLUMN(result, 0, {8}));

	REQUIRE_NO_FAIL(con.Query("CREATE MACRO list_first_plus(l, x) AS l[1] + x"));
	result = con.Query("SELECT list_first_plus([10, 20], 1)");
	REQUIRE(CHECK_COLUMN(result, 0, {11}));

	// folded arguments mixed with a column from a table in the remote catalog
	result = con.Query("SELECT list_first_plus([10, 20], arr[2]) FROM s.macro_args_tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {12}));
	result = con.Query("SELECT add_default(arr[1], 2 * 3) FROM s.macro_args_tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));

	// the folded root expression keeps the name DuckDB would generate
	result = con.Query("SELECT column_name FROM (DESCRIBE SELECT [1, 2])");
	REQUIRE(CHECK_COLUMN(result, 0, {"list_value(1, 2)"}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE s.macro_args_tbl"));
}
