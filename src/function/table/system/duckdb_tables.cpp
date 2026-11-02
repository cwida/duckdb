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
	DuckDBTablesData() : offset(0) {
	}

	vector<reference<CatalogEntry>> entries;
	idx_t offset;
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

	return SystemCatalogScanner::CreateBindData(names);
}

unique_ptr<GlobalTableFunctionState> DuckDBTablesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTablesData>();

	// scan the surviving schemas for tables and collect them
	auto schemas = SystemCatalogScanner::GetSchemas(context, input, result->stats);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry); });
	};
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

	// database_name, VARCHAR
	auto &database_name = output.data[0];
	// database_oid, BIGINT
	auto &database_oid = output.data[1];
	// schema_name, VARCHAR
	auto &schema_name = output.data[2];
	// schema_oid, BIGINT
	auto &schema_oid = output.data[3];
	// table_name, VARCHAR
	auto &table_name = output.data[4];
	// table_oid, BIGINT
	auto &table_oid = output.data[5];
	// comment, VARCHAR
	auto &comment = output.data[6];
	// tags, MAP(VARCHAR, VARCHAR)
	auto &tags = output.data[7];
	// internal, BOOLEAN
	auto &internal = output.data[8];
	// temporary, BOOLEAN
	auto &temporary = output.data[9];
	// has_primary_key, BOOLEAN
	auto &has_primary_key = output.data[10];
	// estimated_size, BIGINT
	auto &estimated_size = output.data[11];
	// column_count, BIGINT
	auto &column_count = output.data[12];
	// index_count, BIGINT
	auto &index_count = output.data[13];
	// check_constraint_count, BIGINT
	auto &check_constraint_count = output.data[14];
	// sql, VARCHAR
	auto &sql = output.data[15];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++].get();

		if (entry.type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = entry.Cast<TableCatalogEntry>();
		auto storage_info = table.GetStorageInfo(context);

		database_name.Append(Value(table.catalog.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(table.catalog.GetOid())));
		schema_name.Append(Value(table.schema.name));
		schema_oid.Append(Value::BIGINT(NumericCast<int64_t>(table.schema.oid)));
		table_name.Append(Value(table.name));
		table_oid.Append(Value::BIGINT(NumericCast<int64_t>(table.oid)));
		comment.Append(Value(table.comment));
		tags.Append(Value::MAP(table.tags));
		internal.Append(Value::BOOLEAN(table.internal));
		temporary.Append(Value::BOOLEAN(table.temporary));
		has_primary_key.Append(Value::BOOLEAN(table.HasPrimaryKey()));

		Value card_val = !storage_info.cardinality.IsValid()
		                     ? Value()
		                     : Value::BIGINT(NumericCast<int64_t>(storage_info.cardinality.GetIndex()));
		estimated_size.Append(card_val);
		column_count.Append(Value::BIGINT(NumericCast<int64_t>(table.GetColumns().LogicalColumnCount())));
		index_count.Append(Value::BIGINT(NumericCast<int64_t>(storage_info.index_info.size())));
		check_constraint_count.Append(Value::BIGINT(NumericCast<int64_t>(CheckConstraintCount(table))));
		auto table_info = table.GetInfo();
		table_info->StripCatalogQualification();
		sql.Append(Value(table_info->ToString()));
		count++;
	}
}

static void DuckDBTablesGetMetrics(TableFunctionGetMetricsInput &input) {
	SystemCatalogScanner::ReportMetrics(input, input.global_state->Cast<DuckDBTablesData>().stats);
}

void DuckDBTablesFun::RegisterFunction(BuiltinFunctions &set) {
	auto function = TableFunction("duckdb_tables", {}, DuckDBTablesFunction, DuckDBTablesBind, DuckDBTablesInit);
	SystemCatalogScanner::RegisterPushdown(function);
	function.get_metrics = DuckDBTablesGetMetrics;
	set.AddFunction(std::move(function));
}

} // namespace duckdb
