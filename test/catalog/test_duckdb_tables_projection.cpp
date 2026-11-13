#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct CatalogMetadataCalls {
	idx_t storage = 0;
	idx_t sql = 0;
	optional_idx cardinality = 42;
};

class CountingTableEntry : public TableCatalogEntry {
public:
	CountingTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, CatalogMetadataCalls &calls)
	    : TableCatalogEntry(catalog, schema, info), columns(info.columns.Copy()), calls(calls) {
	}

	const ColumnList &GetColumns() const override {
		return columns;
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return nullptr;
	}

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		throw NotImplementedException("CountingTableEntry only supports catalog queries");
	}

	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		calls.storage++;
		TableStorageInfo result;
		result.cardinality = calls.cardinality;
		result.index_info.emplace_back();
		return result;
	}

	unique_ptr<CreateInfo> GetInfo() const override {
		calls.sql++;
		return TableCatalogEntry::GetInfo();
	}

private:
	ColumnList columns;
	CatalogMetadataCalls &calls;
};

} // namespace

TEST_CASE("duckdb_tables only fetches requested metadata", "[catalog]") {
	CatalogMetadataCalls calls;
	DuckDB db(nullptr);
	Connection con(db);
	con.context->RunFunctionInTransaction([&]() {
		auto &schema = Catalog::GetSchema(*con.context, "temp", "main").Cast<DuckSchemaEntry>();
		CreateTableInfo info(schema, "counting_table");
		info.temporary = true;
		info.tags["owner"] = "fixture";
		info.columns.AddColumn(ColumnDefinition("i", LogicalType::INTEGER));
		info.columns.Finalize();
		auto entry = make_uniq<CountingTableEntry>(schema.catalog, schema, info, calls);
		REQUIRE(schema.AddEntry(schema.GetCatalogTransaction(*con.context), std::move(entry),
		                        OnCreateConflict::ERROR_ON_CONFLICT));
	});
	// Ignore any metadata requests made while installing the test entry.
	calls.storage = 0;
	calls.sql = 0;

	SECTION("Names, predicates, ordering and counts need neither storage information nor SQL") {
		auto result = con.Query("SELECT table_name FROM duckdb_tables() "
		                        "WHERE database_name = 'temp' ORDER BY table_name LIMIT 5");
		REQUIRE(CHECK_COLUMN(result, 0, {"counting_table"}));
		result = con.Query("SELECT count(*) FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		result = con.Query("SELECT 1 FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		REQUIRE(calls.storage == 0);
		REQUIRE(calls.sql == 0);
	}

	SECTION("Both storage columns share one request, including repeated projections") {
		auto result = con.Query("SELECT index_count, estimated_size, index_count FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		REQUIRE(CHECK_COLUMN(result, 1, {42}));
		REQUIRE(CHECK_COLUMN(result, 2, {1}));
		REQUIRE(calls.storage == 1);
		REQUIRE(calls.sql == 0);
	}

	SECTION("Index count alone requires storage information") {
		auto result = con.Query("SELECT index_count FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		REQUIRE(calls.storage == 1);
		REQUIRE(calls.sql == 0);
	}

	SECTION("Projected map values need neither storage information nor SQL") {
		auto result = con.Query("SELECT tags['owner'] FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {"fixture"}));
		REQUIRE(calls.storage == 0);
		REQUIRE(calls.sql == 0);
	}

	SECTION("Unknown cardinality remains NULL") {
		calls.cardinality = optional_idx();
		auto result = con.Query("SELECT estimated_size FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {Value()}));
		REQUIRE(calls.storage == 1);
		REQUIRE(calls.sql == 0);
	}

	SECTION("Storage information is fetched for a predicate without a projected statistic") {
		auto result = con.Query("SELECT table_name FROM duckdb_tables() WHERE estimated_size > 0");
		REQUIRE(CHECK_COLUMN(result, 0, {"counting_table"}));
		REQUIRE(calls.storage == 1);
		REQUIRE(calls.sql == 0);
	}

	SECTION("SQL generation does not require storage information") {
		auto result = con.Query("SELECT sql FROM duckdb_tables()");
		REQUIRE(CHECK_COLUMN(result, 0, {"CREATE TEMP TABLE counting_table(i INTEGER);"}));
		REQUIRE(calls.storage == 0);
		REQUIRE(calls.sql == 1);
	}

	SECTION("Selecting every column still fetches both kinds of metadata") {
		auto result = con.Query("SELECT * FROM duckdb_tables()");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->ColumnCount() == 16);
		REQUIRE(CHECK_COLUMN(result, 11, {42}));
		REQUIRE(CHECK_COLUMN(result, 13, {1}));
		REQUIRE(calls.storage == 1);
		REQUIRE(calls.sql == 1);
	}
}
