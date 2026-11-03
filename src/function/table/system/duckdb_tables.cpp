#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/function/table/system_catalog_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
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
};

static unique_ptr<FunctionData> DuckDBTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
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

	return SystemCatalogScanFunction::Bind();
}

unique_ptr<GlobalTableFunctionState> DuckDBTablesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTablesData>();

	auto schemas = SystemCatalogScanFunction::GetSchemas(context, input.bind_data);
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
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++].get();

		if (entry.type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = entry.Cast<TableCatalogEntry>();
		auto storage_info = table.GetStorageInfo(context);
		// return values:
		idx_t col = 0;
		// database_name, VARCHAR
		output.SetValue(col++, count, table.catalog.GetName());
		// database_oid, BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(table.catalog.GetOid())));
		// schema_name, LogicalType::VARCHAR
		output.SetValue(col++, count, Value(table.schema.name));
		// schema_oid, LogicalType::BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(table.schema.oid)));
		// table_name, LogicalType::VARCHAR
		output.SetValue(col++, count, Value(table.name));
		// table_oid, LogicalType::BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(table.oid)));
		// comment, LogicalType::VARCHAR
		output.SetValue(col++, count, Value(table.comment));
		// tags, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)
		output.SetValue(col++, count, Value::MAP(table.tags));
		// internal, LogicalType::BOOLEAN
		output.SetValue(col++, count, Value::BOOLEAN(table.internal));
		// temporary, LogicalType::BOOLEAN
		output.SetValue(col++, count, Value::BOOLEAN(table.temporary));
		// has_primary_key, LogicalType::BOOLEAN
		output.SetValue(col++, count, Value::BOOLEAN(table.HasPrimaryKey()));
		// estimated_size, LogicalType::BIGINT

		Value card_val = !storage_info.cardinality.IsValid()
		                     ? Value()
		                     : Value::BIGINT(NumericCast<int64_t>(storage_info.cardinality.GetIndex()));
		output.SetValue(col++, count, card_val);
		// column_count, LogicalType::BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(table.GetColumns().LogicalColumnCount())));
		// index_count, LogicalType::BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(storage_info.index_info.size())));
		// check_constraint_count, LogicalType::BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(CheckConstraintCount(table))));
		// sql, LogicalType::VARCHAR
		auto table_info = table.GetInfo();
		table_info->catalog.clear();
		output.SetValue(col++, count, Value(table_info->ToString()));
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBTablesFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction function("duckdb_tables", {}, DuckDBTablesFunction, DuckDBTablesBind, DuckDBTablesInit);
	SystemCatalogScanFunction::Register(function);
	set.AddFunction(std::move(function));
}

} // namespace duckdb
