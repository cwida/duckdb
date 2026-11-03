#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/function/table/system_catalog_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"

namespace duckdb {

struct DuckDBViewsData : public GlobalTableFunctionState {
	DuckDBViewsData() : offset(0) {
	}

	vector<reference<CatalogEntry>> entries;
	vector<ColumnIndex> column_ids;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBViewsBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("view_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("view_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("column_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("is_bound");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return SystemCatalogScanFunction::Bind();
}

unique_ptr<GlobalTableFunctionState> DuckDBViewsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBViewsData>();

	auto schemas = SystemCatalogScanFunction::GetSchemas(context, input.bind_data);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::VIEW_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry); });
	};
	result->column_ids = input.column_indexes;
	return std::move(result);
}

void DuckDBViewsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBViewsData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++].get();

		if (entry.type != CatalogType::VIEW_ENTRY) {
			continue;
		}
		auto &view = entry.Cast<ViewCatalogEntry>();

		for (idx_t c = 0; c < data.column_ids.size(); c++) {
			auto column_id = data.column_ids[c].GetPrimaryIndex();
			switch (column_id) {
			case 0:
				// database_name, VARCHAR
				output.SetValue(c, count, view.catalog.GetName());
				break;
			case 1:
				// database_oid, BIGINT
				output.SetValue(c, count, Value::BIGINT(NumericCast<int64_t>(view.catalog.GetOid())));
				break;
			case 2:
				// schema_name, LogicalType::VARCHAR
				output.SetValue(c, count, Value(view.schema.name));
				break;
			case 3:
				// schema_oid, LogicalType::BIGINT
				output.SetValue(c, count, Value::BIGINT(NumericCast<int64_t>(view.schema.oid)));
				break;
			case 4:
				// view_name, LogicalType::VARCHAR
				output.SetValue(c, count, Value(view.name));
				break;
			case 5:
				// view_oid, LogicalType::BIGINT
				output.SetValue(c, count, Value::BIGINT(NumericCast<int64_t>(view.oid)));
				break;
			case 6:
				// comment, LogicalType::VARCHARs
				output.SetValue(c, count, Value(view.comment));
				break;
			case 7:
				// tags, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)
				output.SetValue(c, count, Value::MAP(view.tags));
				break;
			case 8:
				// internal, LogicalType::BOOLEAN
				output.SetValue(c, count, Value::BOOLEAN(view.internal));
				break;
			case 9:
				// temporary, LogicalType::BOOLEAN
				output.SetValue(c, count, Value::BOOLEAN(view.temporary));
				break;
			case 10: {
				// column_count, LogicalType::BIGINT
				// make sure the view is bound so we know the columns it emits
				auto columns = view.GetColumnInfo();
				Value column_count;
				if (columns) {
					column_count = Value::BIGINT(NumericCast<int64_t>(columns->types.size()));
				}
				output.SetValue(c, count, column_count);
				break;
			}
			case 11:
				// sql, LogicalType::VARCHAR
				output.SetValue(c, count, Value(view.ToSQL()));
				break;
			case 12: {
				// is_bound, LogicalType::BOOLEAN
				auto columns = view.GetColumnInfo();
				output.SetValue(c, count, Value::BOOLEAN(columns.get()));
				break;
			}
			default:
				throw InternalException("Unsupported column index for duckdb_views");
			}
		}
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBViewsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction duckdb_views("duckdb_views", {}, DuckDBViewsFunction, DuckDBViewsBind, DuckDBViewsInit);
	duckdb_views.projection_pushdown = true;
	SystemCatalogScanFunction::Register(duckdb_views);
	set.AddFunction(std::move(duckdb_views));
}

} // namespace duckdb
