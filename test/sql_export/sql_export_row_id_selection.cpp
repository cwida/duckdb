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
#include "duckdb/storage/table_storage_info.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace logical_plan_sql_export_test;

namespace {

//! The row-id layout a catalog exposes: which row-id columns exist and what they are called.
struct RowIdLayout {
	vector<column_t> row_ids;
	virtual_column_map_t virtual_columns;
};

const column_t FILE_COLUMN = COLUMN_IDENTIFIER_ROW_ID - 2;
const column_t FILE_INDEX_COLUMN = COLUMN_IDENTIFIER_ROW_ID - 3;
const column_t POSITION_COLUMN = COLUMN_IDENTIFIER_ROW_ID - 4;

virtual_column_map_t LakehouseVirtualColumns() {
	virtual_column_map_t result;
	result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::BIGINT)));
	result.insert(make_pair(FILE_COLUMN, TableColumn("filename", LogicalType::VARCHAR)));
	result.insert(make_pair(FILE_INDEX_COLUMN, TableColumn("file_index", LogicalType::BIGINT)));
	result.insert(make_pair(POSITION_COLUMN, TableColumn("file_row_number", LogicalType::BIGINT)));
	return result;
}

//! A table entry exposing the row-id layout of a lakehouse catalog, e.g. the logical row id plus
//! physical file coordinates. No in-core catalog produces such a layout through SQL, so the key
//! selection is exercised through the exporter with synthetic layouts.
class MultiRowIdTableEntry : public TableCatalogEntry {
public:
	MultiRowIdTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, RowIdLayout layout_p)
	    : TableCatalogEntry(catalog, schema, info), table_columns(info.columns.Copy()), layout(std::move(layout_p)) {
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
		return layout.row_ids;
	}
	virtual_column_map_t GetVirtualColumns() const override {
		return layout.virtual_columns;
	}

private:
	ColumnList table_columns;
	RowIdLayout layout;
};

unique_ptr<Expression> RowConstant(Value value) {
	return make_uniq<BoundConstantExpression>(std::move(value));
}

//! A distinct source value per coordinate, so a key built from the wrong source slot cannot
//! accidentally carry the logical row id's value.
Value LayoutConstant(column_t row_id) {
	if (row_id == COLUMN_IDENTIFIER_ROW_ID) {
		return Value::BIGINT(1);
	}
	if (row_id == FILE_COLUMN) {
		return Value("f");
	}
	if (row_id == FILE_INDEX_COLUMN) {
		return Value::BIGINT(7);
	}
	if (row_id == POSITION_COLUMN) {
		return Value::BIGINT(0);
	}
	return Value::BIGINT(5);
}

//! A DELETE whose source projects one aliased reference per row-id column, in the layout's order.
LogicalPlanSQLExportResult ExportDeleteFor(ClientContext &context, MultiRowIdTableEntry &table,
                                           const RowIdLayout &layout) {
	const TableIndex proj_index(100);
	// a row id the layout does not name still needs a source slot, since the refusal under test
	// happens in the exporter rather than in this plan construction
	auto column_for = [&](column_t row_id) {
		auto entry = layout.virtual_columns.find(row_id);
		return entry != layout.virtual_columns.end() ? entry->second : TableColumn("unnamed", LogicalType::BIGINT);
	};
	vector<unique_ptr<Expression>> source_row;
	for (auto &row_id : layout.row_ids) {
		source_row.push_back(RowConstant(LayoutConstant(row_id)));
	}
	auto dummy = make_uniq<LogicalDummyScan>(TableIndex(99));
	auto projection = PlanProjection(proj_index, std::move(dummy), std::move(source_row));
	auto del = make_uniq<LogicalDelete>(table, TableIndex(101));
	for (idx_t i = 0; i < layout.row_ids.size(); i++) {
		auto column = column_for(layout.row_ids[i]);
		auto ref = make_uniq<BoundColumnRefExpression>(column.type, ColumnBinding(proj_index, ProjectionIndex(i)));
		ref->SetAlias(column.name);
		del->expressions.push_back(std::move(ref));
	}
	del->AddChild(std::move(projection));
	del->ResolveOperatorTypes();
	return LogicalPlanSQLExporter::Export(context, *del);
}

//! The new value first (UPDATE and MERGE read their inputs there), then one constant per row-id
//! column in the catalog's order: the logical row id targets the real table's second row, and the
//! physical position deliberately names the first row so a positional key would modify the wrong row.
vector<unique_ptr<Expression>> MultiRowIdSourceRow(const RowIdLayout &layout) {
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(RowConstant(Value::INTEGER(99)));
	for (auto &row_id : layout.row_ids) {
		expressions.push_back(RowConstant(LayoutConstant(row_id)));
	}
	return expressions;
}

} // namespace

// The multi-row-id layouts cannot be reached through SQL: every in-core catalog exposes exactly one
// row-id column. These shapes pin which column, if any, keys the exported statement.
TEST_CASE("Exported row-id key selection", "[sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE layout_target(i INTEGER, v INTEGER)"));

	// catalog access and the export need an active transaction
	connection.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*connection.context, "memory");
	auto &schema = catalog.GetSchema(*connection.context, Identifier("main"));
	CreateTableInfo info(schema, Identifier("layout_target"));
	info.columns.AddColumn(ColumnDefinition(Identifier("i"), LogicalType::INTEGER));
	info.columns.AddColumn(ColumnDefinition(Identifier("v"), LogicalType::INTEGER));

	auto export_for = [&](RowIdLayout layout) {
		MultiRowIdTableEntry table(catalog, schema, info, layout);
		return ExportDeleteFor(*connection.context, table, layout);
	};

	SECTION("a single row-id column keys the statement by itself") {
		auto exported = export_for({{COLUMN_IDENTIFIER_ROW_ID}, LakehouseVirtualColumns()});
		REQUIRE(exported.IsSuccess());
		REQUIRE(exported.GetValue().query->ToString().find("rowid") != string::npos);
	}

	SECTION("a single non-logical row-id column also keys the statement") {
		auto exported = export_for({{FILE_COLUMN}, LakehouseVirtualColumns()});
		REQUIRE(exported.IsSuccess());
		REQUIRE(exported.GetValue().query->ToString().find("filename") != string::npos);
	}

	SECTION("the logical row id is selected among several row-id columns") {
		auto exported =
		    export_for({{FILE_COLUMN, POSITION_COLUMN, COLUMN_IDENTIFIER_ROW_ID}, LakehouseVirtualColumns()});
		REQUIRE(exported.IsSuccess());
		auto text = exported.GetValue().query->ToString();
		REQUIRE(text.find("rowid") != string::npos);
	}

	SECTION("several row-id columns without a logical row id have no key") {
		auto exported = export_for({{FILE_COLUMN, POSITION_COLUMN}, LakehouseVirtualColumns()});
		REQUIRE_FALSE(exported.IsSuccess());
	}

	SECTION("an empty row-id set has no key") {
		auto exported = export_for({{}, LakehouseVirtualColumns()});
		REQUIRE_FALSE(exported.IsSuccess());
	}

	SECTION("a selected row id missing from the virtual columns has no key") {
		virtual_column_map_t without_rowid;
		without_rowid.insert(make_pair(FILE_COLUMN, TableColumn("filename", LogicalType::VARCHAR)));
		auto exported = export_for({{COLUMN_IDENTIFIER_ROW_ID}, without_rowid});
		REQUIRE_FALSE(exported.IsSuccess());
	}

	SECTION("a physical column shadowing the row-id name has no key") {
		virtual_column_map_t shadowed;
		shadowed.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("i", LogicalType::BIGINT)));
		auto exported = export_for({{COLUMN_IDENTIFIER_ROW_ID}, shadowed});
		REQUIRE_FALSE(exported.IsSuccess());
	}
}

TEST_CASE("Exported row-id position with physical coordinates around the logical id", "[sql_export]") {
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

	const RowIdLayout singleton_layout {{COLUMN_IDENTIFIER_ROW_ID}, LakehouseVirtualColumns()};
	const RowIdLayout leading_layout {{COLUMN_IDENTIFIER_ROW_ID, FILE_COLUMN, FILE_INDEX_COLUMN, POSITION_COLUMN},
	                                  LakehouseVirtualColumns()};
	const RowIdLayout trailing_three_layout {{FILE_COLUMN, POSITION_COLUMN, COLUMN_IDENTIFIER_ROW_ID},
	                                         LakehouseVirtualColumns()};
	const RowIdLayout trailing_layout {{FILE_COLUMN, FILE_INDEX_COLUMN, POSITION_COLUMN, COLUMN_IDENTIFIER_ROW_ID},
	                                   LakehouseVirtualColumns()};

	const TableIndex proj_index(100);

	auto run_update = [&](const RowIdLayout &layout) {
		MultiRowIdTableEntry table(catalog, schema, info, layout);
		auto dummy = make_uniq<LogicalDummyScan>(TableIndex(99));
		auto projection = PlanProjection(proj_index, std::move(dummy), MultiRowIdSourceRow(layout));
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
	};

	SECTION("UPDATE keys the leading logical row id, not the trailing position") {
		run_update(leading_layout);
	}

	SECTION("UPDATE keys the trailing logical row id, not the leading position") {
		run_update(trailing_layout);
	}

	SECTION("UPDATE keys a single row-id column with count one") {
		run_update(singleton_layout);
	}

	SECTION("UPDATE keys the trailing logical row id among three row-id columns") {
		run_update(trailing_three_layout);
	}

	SECTION("MERGE accepts the full row-id set and keys the logical row id") {
		MultiRowIdTableEntry table(catalog, schema, info, leading_layout);
		auto dummy = make_uniq<LogicalDummyScan>(TableIndex(99));
		auto projection = PlanProjection(proj_index, std::move(dummy), MultiRowIdSourceRow(leading_layout));
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
		MultiRowIdTableEntry table(catalog, schema, info, leading_layout);
		auto exported = ExportDeleteFor(*connection.context, table, leading_layout);
		REQUIRE(exported.IsSuccess());
		connection.Rollback();
		REQUIRE_NO_FAIL(*connection.Query(exported.GetValue().query->ToString()));
		auto result = connection.Query("SELECT i FROM rowid_layout");
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(10));
	}
}
