#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

//! A table whose scans expose more virtual columns than its delete can return, as storage extensions do
//! (e.g. file coordinates that are only known to the scan).
class ScanOnlyVirtualColumnsTableEntry : public TableCatalogEntry {
public:
	ScanOnlyVirtualColumnsTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
	    : TableCatalogEntry(catalog, schema, info), table_columns(info.columns.Copy()) {
	}

	const ColumnList &GetColumns() const override {
		return table_columns;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return nullptr;
	}
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		throw InternalException("ScanOnlyVirtualColumnsTableEntry has no scan function");
	}
	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		return TableStorageInfo();
	}
	virtual_column_map_t GetVirtualColumns() const override {
		auto result = GetDeleteReturningVirtualColumns();
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID - 2, TableColumn("filename", LogicalType::VARCHAR)));
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID - 3, TableColumn("file_row_number", LogicalType::BIGINT)));
		return result;
	}
	virtual_column_map_t GetDeleteReturningVirtualColumns() const override {
		virtual_column_map_t result;
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::BIGINT)));
		return result;
	}

private:
	ColumnList table_columns;
};

} // namespace

TEST_CASE("DELETE RETURNING exposes the delete virtual columns rather than the scan virtual columns", "[api]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*connection.context, "memory");
	auto &schema = catalog.GetSchema(*connection.context, Identifier("main"));
	CreateTableInfo info(schema, Identifier("remote_table"));
	info.columns.AddColumn(ColumnDefinition(Identifier("i"), LogicalType::INTEGER));
	info.columns.AddColumn(ColumnDefinition(Identifier("s"), LogicalType::VARCHAR));
	ScanOnlyVirtualColumnsTableEntry table(catalog, schema, info);
	REQUIRE(table.GetVirtualColumns().size() == 3);

	LogicalDelete del(table, TableIndex(1));
	del.return_chunk = true;
	del.ResolveOperatorTypes();
	REQUIRE(del.types == vector<LogicalType> {LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT});
	REQUIRE(static_cast<LogicalOperator &>(del).GetColumnBindings().size() == 3);
	connection.Rollback();
}
