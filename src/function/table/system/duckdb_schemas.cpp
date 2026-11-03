#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/function/table/system_catalog_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"

namespace duckdb {

struct DuckDBSchemasData : public GlobalTableFunctionState {
	DuckDBSchemasData() : offset(0) {
	}

	vector<reference<SchemaCatalogEntry>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBSchemasBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	return SystemCatalogScanFunction::Bind();
}

unique_ptr<GlobalTableFunctionState> DuckDBSchemasInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSchemasData>();

	result->entries = SystemCatalogScanFunction::GetSchemas(context, input.bind_data);

	return std::move(result);
}

void DuckDBSchemasFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSchemasData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset].get();

		// return values:
		idx_t col = 0;
		// "oid", PhysicalType::BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(entry.oid)));
		// database_name, VARCHAR
		output.SetValue(col++, count, entry.catalog.GetName());
		// database_oid, BIGINT
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(entry.catalog.GetOid())));
		// "schema_name", PhysicalType::VARCHAR
		output.SetValue(col++, count, Value(entry.name));
		// "comment", PhysicalType::VARCHAR
		output.SetValue(col++, count, Value(entry.comment));
		// "tags", MAP(VARCHAR, VARCHAR)
		output.SetValue(col++, count, Value::MAP(entry.tags));
		// "internal", PhysicalType::BOOLEAN
		output.SetValue(col++, count, Value::BOOLEAN(entry.internal));
		// "sql", PhysicalType::VARCHAR
		output.SetValue(col++, count, Value());

		data.offset++;
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBSchemasFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction function("duckdb_schemas", {}, DuckDBSchemasFunction, DuckDBSchemasBind, DuckDBSchemasInit);
	SystemCatalogScanFunction::Register(function);
	set.AddFunction(std::move(function));
}

} // namespace duckdb
