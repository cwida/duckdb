#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"

using namespace duckdb;

// Test to see if extensions can be loaded with their normal name and aliases.

struct DummyStorageExtension : StorageExtension {
	DummyStorageExtension() {
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

TEST_CASE("Test storage extension lookup full-name", "[api]") {
	DBConfig config;

	// Register a storage extension under its CANONICAL name "sqlite_scanner"
	// This mimics how the real sqlite_scanner extension registers itself
	// There is a hard-coded alias "sqlite" for "sqlite_scanner"
	StorageExtension::Register(config, "sqlite_scanner", make_shared_ptr<DummyStorageExtension>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	// this works since it is the full name
	auto query = string("ATTACH ':memory:' AS db1 (TYPE SQLITE_SCANNER)");
	auto result = con.Query(query);
	if (result->HasError()) {
		FAIL("Query failed even though sqlite_scanner is registered."
		     "Query: " +
		     query + "\n" + "Error: " + result->GetError());
	}
}

TEST_CASE("Test storage extension lookup alias", "[api]") {
	DBConfig config;

	// Register a storage extension under its CANONICAL name "sqlite_scanner"
	// This mimics how the real sqlite_scanner extension registers itself
	// there is a hard-coded alias "sqlite" for "sqlite_scanner"
	StorageExtension::Register(config, "sqlite_scanner", make_shared_ptr<DummyStorageExtension>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	// Without ApplyExtensionAlias in database_manager.cpp,
	// this fails with an error about not finding the extension
	auto query = string("ATTACH ':memory:' AS db1 (TYPE SQLITE)");
	auto result = con.Query(query);
	if (result->HasError()) {
		FAIL("Query failed even though sqlite_scanner is registered.\n"
		     "Query: " +
		     query + "\n" + "Error: " + result->GetError());
	}
}

//! A transaction manager that is not backed by DuckDB storage. Extensions such as sqlite_scanner or
//! ducklake attach catalogs whose transactions are not DuckTransactions.
struct DummyNonDuckTransactionManager : TransactionManager {
	explicit DummyNonDuckTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		transaction = make_uniq<Transaction>(*this, context);
		return *transaction;
	}
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		return ErrorData();
	}
	void RollbackTransaction(Transaction &transaction) override {
	}
	void Checkpoint(ClientContext &context, bool force = false) override {
	}

	unique_ptr<Transaction> transaction;
};

struct DummyNonDuckStorageExtension : StorageExtension {
	DummyNonDuckStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DummyNonDuckTransactionManager>(db);
		};
	}
};

TEST_CASE("Statements executed while a non-DuckDB catalog is attached", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "dummy_non_duckdb", make_shared_ptr<DummyNonDuckStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS dummy_db (TYPE DUMMY_NON_DUCKDB)"));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));

	// mimic extensions such as sqlite_scanner, which have a non-DuckDB transaction open for their catalog
	auto &attached = Catalog::GetCatalog(*con.context, "dummy_db").GetAttached();
	Transaction::Get(*con.context, attached);

	// statements in the main database must keep working while the non-DuckDB catalog is attached
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE parent(i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE child(j INTEGER, FOREIGN KEY (j) REFERENCES parent(i))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO parent VALUES (1), (2)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO child VALUES (1), (2)"));

	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE_NO_FAIL(con.Query("DETACH dummy_db"));
}
