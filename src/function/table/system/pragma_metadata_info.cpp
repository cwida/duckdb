#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/function/function_set.hpp"
namespace duckdb {

struct PragmaMetadataFunctionData : public TableFunctionData {
	explicit PragmaMetadataFunctionData() {
	}

	vector<MetadataBlockInfo> metadata_info;
};

struct PragmaMetadataOperatorData : public GlobalTableFunctionState {
	PragmaMetadataOperatorData() : offset(0) {
	}

	idx_t offset;
};

static unique_ptr<FunctionData> PragmaMetadataInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("block_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("total_blocks");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("free_blocks");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("free_list");
	return_types.emplace_back(LogicalType::LIST(LogicalType::BIGINT));

	Identifier db_name;
	if (input.inputs.empty()) {
		db_name = DatabaseManager::GetDefaultDatabase(context);
	} else {
		if (input.inputs[0].IsNull()) {
			throw BinderException("Database argument for pragma_metadata_info cannot be NULL");
		}
		db_name = Identifier(StringValue::Get(input.inputs[0]));
	}
	auto &catalog = Catalog::GetCatalog(context, db_name);
	if (catalog.GetAttached().IsSystem() || catalog.GetAttached().IsTemporary()) {
		throw BinderException("Database \"%s\" does not have storage", db_name.GetIdentifierName());
	}
	auto result = make_uniq<PragmaMetadataFunctionData>();
	result->metadata_info = catalog.GetMetadataInfo(context);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> PragmaMetadataInfoInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<PragmaMetadataOperatorData>();
}

static void PragmaMetadataInfoFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<PragmaMetadataFunctionData>();
	auto &data = data_p.global_state->Cast<PragmaMetadataOperatorData>();
	idx_t count = 0;

	// block_id
	auto &block_id = output.data[0];
	// total_blocks
	auto &total_blocks = output.data[1];
	// free_blocks
	auto &free_blocks = output.data[2];
	// free_list
	auto &free_list = output.data[3];

	while (data.offset < bind_data.metadata_info.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = bind_data.metadata_info[data.offset++];

		block_id.Append(Value::BIGINT(entry.block_id));
		total_blocks.Append(Value::BIGINT(NumericCast<int64_t>(entry.total_blocks)));
		free_blocks.Append(Value::BIGINT(NumericCast<int64_t>(entry.free_list.size())));
		vector<Value> list_values;
		for (auto &free_id : entry.free_list) {
			list_values.push_back(Value::BIGINT(NumericCast<int64_t>(free_id)));
		}
		free_list.Append(Value::LIST(LogicalType::BIGINT, std::move(list_values)));
		count++;
	}
}

void PragmaMetadataInfo::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet metadata_info("pragma_metadata_info");
	metadata_info.AddFunction(
	    TableFunction({}, PragmaMetadataInfoFunction, PragmaMetadataInfoBind, PragmaMetadataInfoInit));
	metadata_info.AddFunction(TableFunction(FunctionSignature().AddPositionalOnly("database", LogicalType::VARCHAR),
	                                        PragmaMetadataInfoFunction, PragmaMetadataInfoBind,
	                                        PragmaMetadataInfoInit));
	set.AddFunction(metadata_info);
}

} // namespace duckdb
