#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

struct DuckDBTriggersData : public GlobalTableFunctionState {
	DuckDBTriggersData() : offset(0) {
	}

	vector<reference<TriggerCatalogEntry>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBTriggersBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("trigger_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("trigger_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("action_timing");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("event_manipulation");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("columns");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("for_each");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBTriggersInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTriggersData>();

	auto schemas = Catalog::GetAllSchemas(context);
	vector<reference<TableCatalogEntry>> tables;
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type != CatalogType::TABLE_ENTRY) {
				return;
			}
			tables.push_back(entry.Cast<TableCatalogEntry>());
		});
	}
	for (auto &table : tables) {
		auto &table_entry = table.get();
		auto transaction = table_entry.ParentCatalog().GetCatalogTransaction(context);
		table_entry.ScanTriggers(transaction, [&](CatalogEntry &trigger) {
			result->entries.push_back(trigger.Cast<TriggerCatalogEntry>());
		});
	}
	return std::move(result);
}

void DuckDBTriggersFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBTriggersData>();
	if (data.offset >= data.entries.size()) {
		return;
	}

	idx_t count = 0;

	auto &database_name = output.data[0];
	auto &database_oid = output.data[1];
	auto &schema_name = output.data[2];
	auto &schema_oid = output.data[3];
	auto &trigger_name = output.data[4];
	auto &trigger_oid = output.data[5];
	auto &table_name = output.data[6];
	auto &action_timing = output.data[7];
	auto &event_manipulation = output.data[8];
	auto &columns = output.data[9];
	auto &for_each = output.data[10];
	auto &comment = output.data[11];
	auto &tags = output.data[12];
	auto &temporary = output.data[13];
	auto &sql = output.data[14];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &trigger = data.entries[data.offset++].get();

		database_name.Append(Value(trigger.catalog.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(trigger.catalog.GetOid())));
		schema_name.Append(Value(trigger.schema.name));
		schema_oid.Append(Value::BIGINT(NumericCast<int64_t>(trigger.schema.oid)));
		trigger_name.Append(Value(trigger.name));
		trigger_oid.Append(Value::BIGINT(NumericCast<int64_t>(trigger.oid)));
		table_name.Append(Value(trigger.base_table->Table()));
		action_timing.Append(Value(EnumUtil::ToString(trigger.timing)));
		event_manipulation.Append(Value(EnumUtil::ToString(trigger.event_type)));
		vector<Value> col_vals;
		col_vals.reserve(trigger.columns.size());
		for (auto &col_name : trigger.columns) {
			col_vals.emplace_back(col_name);
		}
		columns.Append(Value::LIST(LogicalType::VARCHAR, std::move(col_vals)));
		for_each.Append(Value(EnumUtil::ToString(trigger.for_each)));
		comment.Append(Value(trigger.comment));
		tags.Append(Value::MAP(trigger.tags));
		temporary.Append(Value::BOOLEAN(trigger.temporary));
		sql.Append(Value(trigger.ToSQL()));

		count++;
	}
}

void DuckDBTriggersFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("duckdb_triggers", {}, DuckDBTriggersFunction, DuckDBTriggersBind, DuckDBTriggersInit));
}

} // namespace duckdb
