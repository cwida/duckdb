#include "catch.hpp"
#include "logical_plan_sql_export_test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace logical_plan_sql_export_test;

// The multi-row-id selection cannot be reached through SQL: every in-core catalog exposes exactly one
// row-id column. Catalogs backing lakehouse tables return several (the logical row id plus physical
// file coordinates), so the selection logic is exercised here with synthetic row-id sets.
TEST_CASE("Exported row-id key selection", "[sql_export]") {
	const column_t file_column = COLUMN_IDENTIFIER_ROW_ID - 2;
	const column_t position_column = COLUMN_IDENTIFIER_ROW_ID - 3;

	virtual_column_map_t virtual_columns;
	virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::BIGINT)));
	virtual_columns.insert(make_pair(file_column, TableColumn("filename", LogicalType::VARCHAR)));
	virtual_columns.insert(make_pair(position_column, TableColumn("file_row_number", LogicalType::BIGINT)));

	SECTION("a single row-id column keys the statement by itself") {
		vector<column_t> row_ids {COLUMN_IDENTIFIER_ROW_ID};
		auto selected = SQLExportDMLHelpers::SelectRowIdColumn(row_ids, virtual_columns);
		REQUIRE(selected.column);
		REQUIRE(selected.column->name == Identifier("rowid"));
		REQUIRE(selected.ordinal == 0);
		REQUIRE(selected.count == 1);
	}

	SECTION("a single non-logical row-id column also keys the statement") {
		vector<column_t> row_ids {file_column};
		auto selected = SQLExportDMLHelpers::SelectRowIdColumn(row_ids, virtual_columns);
		REQUIRE(selected.column);
		REQUIRE(selected.column->name == Identifier("filename"));
		REQUIRE(selected.ordinal == 0);
		REQUIRE(selected.count == 1);
	}

	SECTION("the logical row id is selected among several row-id columns") {
		vector<column_t> row_ids {file_column, position_column, COLUMN_IDENTIFIER_ROW_ID};
		auto selected = SQLExportDMLHelpers::SelectRowIdColumn(row_ids, virtual_columns);
		REQUIRE(selected.column);
		REQUIRE(selected.column->name == Identifier("rowid"));
		REQUIRE(selected.ordinal == 2);
		REQUIRE(selected.count == 3);
	}

	SECTION("the leading logical row id keeps ordinal zero among several") {
		vector<column_t> row_ids {COLUMN_IDENTIFIER_ROW_ID, file_column, position_column};
		auto selected = SQLExportDMLHelpers::SelectRowIdColumn(row_ids, virtual_columns);
		REQUIRE(selected.column);
		REQUIRE(selected.column->name == Identifier("rowid"));
		REQUIRE(selected.ordinal == 0);
		REQUIRE(selected.count == 3);
	}

	SECTION("several row-id columns without a logical row id have no key") {
		vector<column_t> row_ids {file_column, position_column};
		REQUIRE_FALSE(SQLExportDMLHelpers::SelectRowIdColumn(row_ids, virtual_columns).column);
	}

	SECTION("an empty row-id set has no key") {
		vector<column_t> row_ids;
		REQUIRE_FALSE(SQLExportDMLHelpers::SelectRowIdColumn(row_ids, virtual_columns).column);
	}

	SECTION("a selected row id missing from the virtual columns has no key") {
		vector<column_t> row_ids {COLUMN_IDENTIFIER_ROW_ID};
		virtual_column_map_t without_rowid;
		without_rowid.insert(make_pair(file_column, TableColumn("filename", LogicalType::VARCHAR)));
		REQUIRE_FALSE(SQLExportDMLHelpers::SelectRowIdColumn(row_ids, without_rowid).column);
	}
}

namespace {

//! A table entry exposing the row-id layout of a lakehouse catalog: the logical row id first,
//! physical file coordinates after it. No in-core catalog produces this layout through SQL.
class MultiRowIdTableEntry : public TableCatalogEntry {
public:
	MultiRowIdTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
	    : TableCatalogEntry(catalog, schema, info), table_columns(info.columns.Copy()) {
	}

	const ColumnList &GetColumns() const override {
		return table_columns;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return nullptr;
	}
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		throw InternalException("MultiRowIdTableEntry has no scan function");
	}
	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		return TableStorageInfo();
	}
	vector<column_t> GetRowIdColumns() const override {
		return {COLUMN_IDENTIFIER_ROW_ID, COLUMN_IDENTIFIER_ROW_ID - 2, COLUMN_IDENTIFIER_ROW_ID - 3,
		        COLUMN_IDENTIFIER_ROW_ID - 4};
	}
	virtual_column_map_t GetVirtualColumns() const override {
		virtual_column_map_t result;
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::BIGINT)));
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID - 2, TableColumn("filename", LogicalType::VARCHAR)));
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID - 3, TableColumn("file_index", LogicalType::BIGINT)));
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID - 4, TableColumn("file_row_number", LogicalType::BIGINT)));
		return result;
	}

private:
	ColumnList table_columns;
};

unique_ptr<Expression> RowConstant(Value value) {
	return make_uniq<BoundConstantExpression>(std::move(value));
}

//! The new value first (UPDATE and MERGE read their inputs there), then the four row-id columns in
//! the catalog's order: the logical row id targets the real table's second row, and the trailing
//! physical position deliberately names the first row so a positional key would modify the wrong row.
vector<unique_ptr<Expression>> MultiRowIdSourceRow() {
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(RowConstant(Value::INTEGER(99)));
	expressions.push_back(RowConstant(Value::BIGINT(1)));
	expressions.push_back(RowConstant(Value("f")));
	expressions.push_back(RowConstant(Value::BIGINT(7)));
	expressions.push_back(RowConstant(Value::BIGINT(0)));
	return expressions;
}

} // namespace

TEST_CASE("Exported row-id position with physical coordinates after the logical id", "[sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE rowid_layout(i INTEGER, v INTEGER)"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO rowid_layout VALUES (10, 0), (20, 0)"));

	// catalog access and the export need an active transaction, execution of the generated text does not
	connection.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*connection.context, "memory");
	auto &schema = catalog.GetSchema(*connection.context, Identifier("main"));
	CreateTableInfo info(schema, Identifier("rowid_layout"));
	info.columns.AddColumn(ColumnDefinition(Identifier("i"), LogicalType::INTEGER));
	info.columns.AddColumn(ColumnDefinition(Identifier("v"), LogicalType::INTEGER));
	MultiRowIdTableEntry table(catalog, schema, info);

	const TableIndex proj_index(100);

	SECTION("UPDATE keys the logical row id, not the trailing position") {
		auto dummy = make_uniq<LogicalDummyScan>(TableIndex(99));
		auto projection = PlanProjection(proj_index, std::move(dummy), MultiRowIdSourceRow());
		auto update = make_uniq<LogicalUpdate>(table);
		update->table_index = TableIndex(101);
		update->columns.push_back(PhysicalIndex(1));
		update->expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(proj_index, ProjectionIndex(0))));
		update->AddChild(std::move(projection));
		update->ResolveOperatorTypes();

		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *update);
		REQUIRE(exported.IsSuccess());
		connection.Rollback();
		REQUIRE_NO_FAIL(*connection.Query(exported.GetValue().query->ToString()));
		auto result = connection.Query("SELECT v FROM rowid_layout ORDER BY i");
		REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(0));
		REQUIRE(result->Collection().GetValue(0, 1) == Value::INTEGER(99));
	}

	SECTION("MERGE accepts the full row-id set and keys the logical row id") {
		auto dummy = make_uniq<LogicalDummyScan>(TableIndex(99));
		auto projection = PlanProjection(proj_index, std::move(dummy), MultiRowIdSourceRow());
		auto merge = make_uniq<LogicalMergeInto>(table);
		merge->table_index = TableIndex(101);
		merge->row_id_start = 1;
		auto action = make_uniq<BoundMergeIntoAction>();
		action->action_type = MergeActionType::MERGE_UPDATE;
		action->columns.push_back(PhysicalIndex(1));
		action->expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(proj_index, ProjectionIndex(0))));
		vector<unique_ptr<BoundMergeIntoAction>> actions;
		actions.push_back(std::move(action));
		merge->actions.emplace(MergeActionCondition::WHEN_MATCHED, std::move(actions));
		merge->AddChild(std::move(projection));
		merge->ResolveOperatorTypes();

		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *merge);
		REQUIRE(exported.IsSuccess());
		connection.Rollback();
		REQUIRE_NO_FAIL(*connection.Query(exported.GetValue().query->ToString()));
		auto result = connection.Query("SELECT v FROM rowid_layout ORDER BY i");
		REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(0));
		REQUIRE(result->Collection().GetValue(0, 1) == Value::INTEGER(99));
	}

	SECTION("DELETE finds the logical row id among the aliased references") {
		auto source_row = MultiRowIdSourceRow();
		source_row.erase(source_row.begin());
		auto dummy = make_uniq<LogicalDummyScan>(TableIndex(99));
		auto projection = PlanProjection(proj_index, std::move(dummy), std::move(source_row));
		auto del = make_uniq<LogicalDelete>(table, TableIndex(101));
		const char *names[] = {"rowid", "filename", "file_index", "file_row_number"};
		for (idx_t i = 0; i < 4; i++) {
			auto type = i == 1 ? LogicalType::VARCHAR : LogicalType::BIGINT;
			auto ref = make_uniq<BoundColumnRefExpression>(type, ColumnBinding(proj_index, ProjectionIndex(i)));
			ref->SetAlias(Identifier(names[i]));
			del->expressions.push_back(std::move(ref));
		}
		del->AddChild(std::move(projection));
		del->ResolveOperatorTypes();

		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *del);
		REQUIRE(exported.IsSuccess());
		connection.Rollback();
		REQUIRE_NO_FAIL(*connection.Query(exported.GetValue().query->ToString()));
		auto result = connection.Query("SELECT i FROM rowid_layout");
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(10));
	}
}
