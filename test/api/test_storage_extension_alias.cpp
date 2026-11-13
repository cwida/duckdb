#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"

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

class DummyExternalTableEntry : public TableCatalogEntry {
public:
	DummyExternalTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
	    : TableCatalogEntry(catalog, schema, info) {
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &, column_t) override {
		return nullptr;
	}

	TableFunction GetScanFunction(ClientContext &, unique_ptr<FunctionData> &) override {
		throw NotImplementedException("Dummy external tables cannot be scanned");
	}

	TableStorageInfo GetStorageInfo(ClientContext &) override {
		return {};
	}
};

class DummyExternalSchemaEntry : public DuckSchemaEntry {
public:
	DummyExternalSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : DuckSchemaEntry(catalog, info) {
	}

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override {
		auto table = make_uniq<DummyExternalTableEntry>(catalog, *this, info.Base());
		return AddEntry(transaction, std::move(table), info.Base().on_conflict);
	}
};

class DummyExternalCatalog : public DuckCatalog {
public:
	explicit DummyExternalCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

	void ReplaceDefaultSchema(CatalogTransaction transaction) {
		auto &schemas = GetSchemaCatalogSet();
		(void)schemas.DropEntry(transaction, DEFAULT_SCHEMA, true, true);

		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName({Identifier::DefaultSchema()}, Identifier()));
		info.internal = true;

		LogicalDependencyList dependencies;
		auto schema = make_uniq<DummyExternalSchemaEntry>(*this, info);
		REQUIRE(schemas.CreateEntry(transaction, DEFAULT_SCHEMA, std::move(schema), dependencies));
	}
};

struct DummyExternalStorageExtension : StorageExtension {
	DummyExternalStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			auto catalog = make_uniq<DummyExternalCatalog>(db);
			catalog->Initialize(false);
			catalog->ReplaceDefaultSchema(CatalogTransaction::GetSystemTransaction(db.GetDatabase()));
			return std::move(catalog);
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

TEST_CASE("Checkpoint skips extension-owned table entries", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "dummy_external", make_shared_ptr<DummyExternalStorageExtension>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS external_db (TYPE dummy_external)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE external_db.tbl(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT external_db"));
}
