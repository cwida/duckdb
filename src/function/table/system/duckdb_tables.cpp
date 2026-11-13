#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table/system_catalog_scanner.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

struct DuckDBTablesData : public GlobalTableFunctionState {
	DuckDBTablesData() : offset(0), needs_storage_info(false) {
	}

	vector<reference<CatalogEntry>> entries;
	vector<ColumnIndex> column_ids;
	idx_t offset;
	bool needs_storage_info;
	SystemCatalogScannerStats stats;
};

static unique_ptr<FunctionData> DuckDBTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("table_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("has_primary_key");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("estimated_size");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("index_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("check_constraint_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	return SystemCatalogScanner::CreateBindData(names, return_types, "table_name", "table_oid");
}

unique_ptr<GlobalTableFunctionState> DuckDBTablesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTablesData>();

	// collect the tables that survive the pushed-down filters
	result->entries = SystemCatalogScanner::GetEntries(context, input, CatalogType::TABLE_ENTRY, result->stats);
	result->column_ids = input.column_indexes;
	for (auto &column_id : result->column_ids) {
		// estimated_size and index_count require storage information.
		if (column_id.GetPrimaryIndex() == 11 || column_id.GetPrimaryIndex() == 13) {
			result->needs_storage_info = true;
		}
	}
	return std::move(result);
}

static idx_t CheckConstraintCount(TableCatalogEntry &table) {
	idx_t check_count = 0;
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type == ConstraintType::CHECK) {
			check_count++;
		}
	}
	return check_count;
}

void DuckDBTablesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBTablesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++].get();

		if (entry.type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = entry.Cast<TableCatalogEntry>();
		TableStorageInfo storage_info;
		if (data.needs_storage_info) {
			storage_info = table.GetStorageInfo(context);
		}
		for (idx_t c = 0; c < data.column_ids.size(); c++) {
			auto column_id = data.column_ids[c].GetPrimaryIndex();
			auto &col_vector = output.data[c];
			switch (column_id) {
			case 0:
				// database_name, VARCHAR
				col_vector.Append(Value(table.catalog.GetName()));
				break;
			case 1:
				// database_oid, BIGINT
				col_vector.Append(Value::BIGINT(NumericCast<int64_t>(table.catalog.GetOid())));
				break;
			case 2:
				// schema_name, VARCHAR
				col_vector.Append(Value(table.schema.name));
				break;
			case 3:
				// schema_oid, BIGINT
				col_vector.Append(Value::BIGINT(NumericCast<int64_t>(table.schema.oid)));
				break;
			case 4:
				// table_name, VARCHAR
				col_vector.Append(Value(table.name));
				break;
			case 5:
				// table_oid, BIGINT
				col_vector.Append(Value::BIGINT(NumericCast<int64_t>(table.oid)));
				break;
			case 6:
				// comment, VARCHAR
				col_vector.Append(Value(table.comment));
				break;
			case 7:
				// tags, MAP(VARCHAR, VARCHAR)
				col_vector.Append(Value::MAP(table.tags));
				break;
			case 8:
				// internal, BOOLEAN
				col_vector.Append(Value::BOOLEAN(table.internal));
				break;
			case 9:
				// temporary, BOOLEAN
				col_vector.Append(Value::BOOLEAN(table.temporary));
				break;
			case 10:
				// has_primary_key, BOOLEAN
				col_vector.Append(Value::BOOLEAN(table.HasPrimaryKey()));
				break;
			case 11: {
				// estimated_size, BIGINT
				Value cardinality = !storage_info.cardinality.IsValid()
				                        ? Value()
				                        : Value::BIGINT(NumericCast<int64_t>(storage_info.cardinality.GetIndex()));
				col_vector.Append(cardinality);
				break;
			}
			case 12:
				// column_count, BIGINT
				col_vector.Append(Value::BIGINT(NumericCast<int64_t>(table.GetColumns().LogicalColumnCount())));
				break;
			case 13:
				// index_count, BIGINT
				col_vector.Append(Value::BIGINT(NumericCast<int64_t>(storage_info.index_info.size())));
				break;
			case 14:
				// check_constraint_count, BIGINT
				col_vector.Append(Value::BIGINT(NumericCast<int64_t>(CheckConstraintCount(table))));
				break;
			case 15: {
				// sql, VARCHAR
				auto table_info = table.GetInfo();
				table_info->StripCatalogQualification();
				col_vector.Append(Value(table_info->ToString()));
				break;
			}
			default:
				throw InternalException("Unsupported column index for duckdb_tables");
			}
		}
		count++;
	}
}

static void DuckDBTablesGetMetrics(TableFunctionGetMetricsInput &input) {
	SystemCatalogScanner::ReportMetrics(input, input.global_state->Cast<DuckDBTablesData>().stats);
}

void DuckDBTablesFun::RegisterFunction(BuiltinFunctions &set) {
	auto function = TableFunction("duckdb_tables", {}, DuckDBTablesFunction, DuckDBTablesBind, DuckDBTablesInit);
	function.projection_pushdown = true;
	SystemCatalogScanner::RegisterPushdown(function);
	function.get_metrics = DuckDBTablesGetMetrics;
	set.AddFunction(std::move(function));
}

} // namespace duckdb
