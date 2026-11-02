#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

struct DuckDBDatabasesData : public GlobalTableFunctionState {
	DuckDBDatabasesData() : offset(0) {
	}

	vector<shared_ptr<AttachedDatabase>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBDatabasesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("path");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("readonly");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("encrypted");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("cipher");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("options");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBDatabasesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBDatabasesData>();

	// scan all the schemas for tables and collect them and collect them
	auto &db_manager = DatabaseManager::Get(context);
	result->entries = db_manager.GetDatabases(context);
	return std::move(result);
}

void DuckDBDatabasesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBDatabasesData>();
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
	// path, VARCHAR
	auto &path = output.data[2];
	// comment, VARCHAR
	auto &comment = output.data[3];
	// tags, MAP
	auto &tags = output.data[4];
	// internal, BOOLEAN
	auto &internal = output.data[5];
	// type, VARCHAR
	auto &type = output.data[6];
	// readonly, BOOLEAN
	auto &readonly = output.data[7];
	// encrypted, BOOLEAN
	auto &encrypted = output.data[8];
	// cipher, VARCHAR
	auto &cipher = output.data[9];
	// options, MAP
	auto &options = output.data[10];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		auto &attached = *entry;
		auto &catalog = attached.GetCatalog();
		if (attached.GetVisibility() == AttachVisibility::HIDDEN) {
			continue;
		}

		database_name.Append(Value(attached.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(attached.oid)));
		bool is_internal = attached.IsSystem() || attached.IsTemporary();
		bool is_readonly = attached.IsReadOnly();
		string cipher_str;
		Value db_path;
		if (!is_internal) {
			bool in_memory = catalog.InMemory();
			if (!in_memory) {
				db_path = Value(catalog.GetDBPath());
			}
			if (catalog.IsEncrypted()) {
				cipher_str = catalog.GetEncryptionCipher();
			}
		}
		path.Append(db_path);
		comment.Append(Value(attached.comment));
		tags.Append(Value::MAP(attached.tags));
		internal.Append(Value::BOOLEAN(is_internal));
		type.Append(Value(catalog.GetCatalogType()));
		readonly.Append(Value::BOOLEAN(is_readonly));
		encrypted.Append(Value::BOOLEAN(catalog.IsEncrypted()));
		cipher.Append(cipher_str.empty() ? Value() : Value(cipher_str));
		InsertionOrderPreservingMap<string> options_map;
		for (const auto &option : attached.GetAttachOptions()) {
			options_map.insert(option.first, option.second.ToString());
		}
		options.Append(Value::MAP(options_map));

		count++;
	}
}

void DuckDBDatabasesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("duckdb_databases", {}, DuckDBDatabasesFunction, DuckDBDatabasesBind, DuckDBDatabasesInit));
}

} // namespace duckdb
