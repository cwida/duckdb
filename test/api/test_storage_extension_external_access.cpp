#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

// Storage extensions do their own I/O - ATTACH is gated centrally on enable_external_access.

struct ExternalAccessStorageExtension : StorageExtension {
	ExternalAccessStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

TEST_CASE("Test storage extension attach with enable_external_access disabled", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "sqlite_scanner", make_shared_ptr<ExternalAccessStorageExtension>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	// attaching through the storage extension works while external access is enabled
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS db1 (TYPE SQLITE_SCANNER)"));

	REQUIRE_NO_FAIL(con.Query("SET enable_external_access=false"));

	auto result = con.Query("ATTACH ':memory:' AS db2 (TYPE SQLITE_SCANNER)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "enable_external_access"));

	// the alias goes through the same path
	result = con.Query("ATTACH ':memory:' AS db2 (TYPE SQLITE)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "enable_external_access"));

	// in-memory duckdb databases are unaffected
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS db3"));
}
