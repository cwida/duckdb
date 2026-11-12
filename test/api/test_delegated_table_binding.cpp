#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

class DelegatedTable : public TableCatalogEntry {
public:
	DelegatedTable(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, TableCatalogEntry &source)
	    : TableCatalogEntry(catalog, schema, info), source(source) {
	}
	const ColumnList &GetColumns() const override {
		return source.GetColumns();
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return source.GetStatistics(context, column_id);
	}
	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		return source.GetStorageInfo(context);
	}
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		return source.GetScanFunction(context, bind_data);
	}
	virtual_column_map_t GetVirtualColumns() const override {
		return source.GetVirtualColumns();
	}

private:
	TableCatalogEntry &source;
};

class DelegatingCatalog : public DuckCatalog {
public:
	explicit DelegatingCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

private:
	CatalogEntryLookup TryLookupEntryInternal(CatalogTransaction transaction, const EntryLookupInfo &lookup) override {
		auto schema_name = lookup.GetSchema().empty() ? Identifier::DefaultSchema() : lookup.GetSchema();
		EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(schema_name));
		auto schema = LookupSchema(transaction, schema_lookup, OnEntryNotFound::RETURN_NULL);
		if (!schema) {
			return {nullptr, nullptr, ErrorData()};
		}
		if (lookup.GetCatalogType() != CatalogType::TABLE_ENTRY || lookup.GetEntryName() != "logical_table") {
			return {schema, schema->LookupEntry(transaction, lookup), ErrorData()};
		}
		EntryLookupInfo source_lookup(CatalogType::TABLE_ENTRY, QualifiedName(Identifier("source"), Identifier("main"),
		                                                                      Identifier("physical_table")));
		auto &source = Catalog::GetEntry(transaction.GetContext(), source_lookup).Cast<TableCatalogEntry>();
		CreateTableInfo info(*schema, Identifier("logical_table"));
		info.columns = source.GetColumns().Copy();
		auto table = make_uniq<DelegatedTable>(*this, *schema, info, source);
		auto result = table.get();
		tables.push_back(std::move(table));
		return {schema, result, ErrorData()};
	}

	vector<unique_ptr<DelegatedTable>> tables;
};

class DelegatingStorage : public StorageExtension {
public:
	DelegatingStorage() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq<DelegatingCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("A delegated scan retains the table name that was resolved", "[catalog][delegated_binding]") {
	DBConfig config;
	StorageExtension::Register(config, "delegating", make_shared_ptr<DelegatingStorage>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto run = [&](const string &sql) {
		auto result = con.Query(sql);
		if (result->HasError()) {
			FAIL(sql + "\n" + result->GetError());
		}
		return result;
	};
	run("ATTACH ':memory:' AS source");
	run("CREATE TABLE source.physical_table AS SELECT 42 AS id");
	run("ATTACH ':memory:' AS routed (TYPE delegating)");
	REQUIRE(
	    run("SELECT routed.main.logical_table.id FROM routed.main.logical_table")->Fetch()->GetValue(0, 0).ToString() ==
	    "42");
	REQUIRE(run("SELECT logical_table.id FROM routed.main.logical_table")->Fetch()->GetValue(0, 0).ToString() == "42");
	REQUIRE(run("SELECT t.id FROM routed.main.logical_table t")->Fetch()->GetValue(0, 0).ToString() == "42");
	run("CREATE VIEW v AS SELECT routed.main.logical_table.id FROM routed.main.logical_table");
	REQUIRE(run("SELECT * FROM v")->Fetch()->GetValue(0, 0).ToString() == "42");
	run("CREATE VIEW nested AS SELECT * FROM v");
	REQUIRE(run("SELECT * FROM nested")->Fetch()->GetValue(0, 0).ToString() == "42");
	run("PREPARE p AS SELECT id FROM routed.main.logical_table");
	REQUIRE(run("EXECUTE p")->Fetch()->GetValue(0, 0).ToString() == "42");
	run("DROP TABLE source.physical_table");
	run("CREATE TABLE source.physical_table AS SELECT 84 AS id");
	REQUIRE(run("EXECUTE p")->Fetch()->GetValue(0, 0).ToString() == "84");
}
