#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

using namespace duckdb;

// A storage extension named by an explicit TYPE owns the interpretation of the ATTACH path. A path such as
// 'http://host:port' may be a connection URI rather than a remote file, so attaching it must neither require httpfs
// nor default to read-only.

namespace {

string last_attached_path;

struct RemotePathStorageExtension : StorageExtension {
	RemotePathStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			// the path is not a file: remember it and back the catalog with an in-memory database
			last_attached_path = info.path;
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("Test storage extension attach with a remote-looking path", "[api]") {
	DBConfig config;
	config.SetOptionByName("autoload_known_extensions", Value::BOOLEAN(false));
	config.SetOptionByName("autoinstall_known_extensions", Value::BOOLEAN(false));
	StorageExtension::Register(config, "remote_path_test", make_shared_ptr<RemotePathStorageExtension>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	// the storage extension receives the path as-is, without httpfs/azure being loaded
	REQUIRE_NO_FAIL(con.Query("ATTACH 'http://localhost:18080' AS r1 (TYPE remote_path_test)"));
	REQUIRE(last_attached_path == "http://localhost:18080");
	REQUIRE_NO_FAIL(con.Query("ATTACH 'https://localhost:18443/catalog' AS r2 (TYPE remote_path_test)"));
	REQUIRE_NO_FAIL(con.Query("ATTACH 's3://bucket/warehouse' AS r3 (TYPE remote_path_test)"));
	REQUIRE_NO_FAIL(con.Query("ATTACH 'azure://container/path' AS r4 (TYPE remote_path_test)"));
	REQUIRE(last_attached_path == "azure://container/path");
	auto result = con.Query("SELECT COUNT(*) FROM duckdb_databases() WHERE database_name IN ('r1', 'r2', 'r3', 'r4')");
	REQUIRE(CHECK_COLUMN(result, 0, {4}));
	// a remote-looking path does not force the database into read-only mode
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE r1.t AS SELECT 42 AS i"));
	result = con.Query("SELECT i FROM r1.t");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	if (!db.ExtensionIsLoaded("httpfs")) {
		// without an explicit storage type, a remote path still requires httpfs
		result = con.Query("ATTACH 'http://localhost:18080/file.duckdb' AS r5");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "httpfs"));
		// ... and so does TYPE duckdb
		result = con.Query("ATTACH 'https://localhost:18443/file.duckdb' AS r6 (TYPE duckdb)");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "httpfs"));
	}
}
