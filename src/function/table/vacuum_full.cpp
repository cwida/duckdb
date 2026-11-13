#include "duckdb/function/table/range.hpp"

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

struct VacuumFullBindData : public FunctionData {
	explicit VacuumFullBindData(Identifier db_name_p) : db_name(std::move(db_name_p)) {
	}

	Identifier db_name;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<VacuumFullBindData>(db_name);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<VacuumFullBindData>();
		return db_name == other.db_name;
	}
};

static Identifier ResolveVacuumFullTarget(ClientContext &context, DatabaseManager &db_manager) {
	auto current_name = DatabaseManager::GetDefaultDatabase(context);
	auto current = db_manager.GetDatabase(current_name);
	if (current && current->HasStorageManager() && !current->GetStorageManager().InMemory()) {
		return current_name;
	}
	Identifier found;
	idx_t persistent_count = 0;
	for (auto &db : db_manager.GetDatabases()) {
		if (db->IsSystem() || db->IsTemporary()) {
			continue;
		}
		if (!db->HasStorageManager() || db->GetStorageManager().InMemory()) {
			continue;
		}
		found = db->GetName();
		persistent_count++;
	}
	if (persistent_count == 1) {
		return found;
	}
	if (persistent_count > 1) {
		throw InvalidInputException(
		    "VACUUM FULL requires the current database to be persistent, or exactly one persistent database to be "
		    "attached");
	}
	throw NotImplementedException("VACUUM FULL is only supported for persistent databases");
}

static unique_ptr<FunctionData> VacuumFullBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	auto &db_manager = DatabaseManager::Get(context);
	return make_uniq<VacuumFullBindData>(ResolveVacuumFullTarget(context, db_manager));
}

static Identifier UniqueAttachedName(DatabaseManager &db_manager, const string &prefix) {
	Identifier candidate(prefix);
	if (!db_manager.GetDatabase(candidate)) {
		return candidate;
	}
	return Identifier(prefix + "_" + UUID::ToString(UUID::GenerateRandomUUID()));
}

static void RunVacuumFullSQL(DatabaseInstance &instance, const string &sql) {
	Connection con(instance);
	auto result = con.Query(sql);
	if (result->HasError()) {
		result->ThrowError();
	}
}

static void RestoreSearchPath(CatalogSearchPath &search_path, const vector<CatalogSearchEntry> &previous_paths) {
	if (previous_paths.empty()) {
		search_path.Reset();
	} else {
		search_path.Set(previous_paths, CatalogSetPathType::SET_DIRECTLY);
	}
}

static void AttachFileDatabase(ClientContext &context, DatabaseManager &db_manager, const Identifier &name,
                               const string &path) {
	AttachInfo info;
	info.name = name;
	info.path = path;
	AttachOptions options(DBConfig::GetConfig(context).options);
	options.access_mode = AccessMode::READ_WRITE;
	db_manager.AttachDatabase(context, info, options);
}

static void TryDetach(ClientContext &context, DatabaseManager &db_manager, const Identifier &name) {
	if (name.empty()) {
		return;
	}
	db_manager.DetachDatabase(context, name, OnEntryNotFound::RETURN_NULL);
}

static void TryRemoveDBFiles(FileSystem &fs, const string &path) {
	fs.TryRemoveFile(path);
	fs.TryRemoveFile(path + ".wal");
}

static void VacuumFullFunctionInternal(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<VacuumFullBindData>();
	auto &db_manager = DatabaseManager::Get(context);
	auto attached = db_manager.GetDatabase(bind_data.db_name);
	if (!attached) {
		throw BinderException("Database \"%s\" not found", bind_data.db_name);
	}
	if (!attached->HasStorageManager() || attached->GetStorageManager().InMemory()) {
		throw NotImplementedException("VACUUM FULL is only supported for persistent databases");
	}
	if (attached->IsReadOnly()) {
		throw InvalidInputException("Cannot run VACUUM FULL on a read-only database");
	}
	if (attached->GetStorageManager().IsEncrypted()) {
		throw NotImplementedException("VACUUM FULL is not yet implemented for encrypted databases");
	}

	const auto path = attached->GetStorageManager().GetDBPath();
	const auto wal_path = attached->GetStorageManager().GetWALPath();
	const auto checkpoint_wal_path = attached->GetStorageManager().GetCheckpointWALPath();
	const auto recovery_wal_path = attached->GetStorageManager().GetRecoveryWALPath();
	attached.reset();

	auto tmp_alias = UniqueAttachedName(db_manager, "__vacuum_full_target");
	auto tmp_path = path + ".vacuum_full.tmp";
	auto backup_path = path + ".vacuum_full.bak";
	auto &fs = FileSystem::GetFileSystem(context);
	TryRemoveDBFiles(fs, tmp_path);
	TryRemoveDBFiles(fs, backup_path);

	Identifier fallback;
	for (auto &db : db_manager.GetDatabases()) {
		if (db->IsSystem() || db->IsTemporary()) {
			continue;
		}
		if (db->GetName() == bind_data.db_name) {
			continue;
		}
		fallback = db->GetName();
		break;
	}

	string copy_sql;
	copy_sql += "CHECKPOINT " + SQLIdentifier(bind_data.db_name) + ";\n";
	copy_sql += "ATTACH " + SQLString(tmp_path) + " AS " + SQLIdentifier(tmp_alias) + ";\n";
	copy_sql += "COPY FROM DATABASE " + SQLIdentifier(bind_data.db_name) + " TO " + SQLIdentifier(tmp_alias) + ";\n";
	copy_sql += "DETACH " + SQLIdentifier(tmp_alias) + ";";
	try {
		RunVacuumFullSQL(DatabaseInstance::GetDatabase(context), copy_sql);
	} catch (...) {
		TryDetach(context, db_manager, tmp_alias);
		TryRemoveDBFiles(fs, tmp_path);
		throw;
	}

	bool created_fallback = false;
	if (fallback.empty()) {
		fallback = UniqueAttachedName(db_manager, "__vacuum_full_switch");
		AttachInfo mem_info;
		mem_info.name = fallback;
		mem_info.path = ":memory:";
		AttachOptions mem_options(DBConfig::GetConfig(context).options);
		db_manager.AttachDatabase(context, mem_info, mem_options);
		created_fallback = true;
	}

	auto &search_path = *ClientData::Get(context).catalog_search_path;
	auto previous_paths = search_path.GetSetPaths();
	search_path.Set(CatalogSearchEntry(fallback, Identifier::DefaultSchema()), CatalogSetPathType::SET_DIRECTLY);

	ErrorData error;
	try {
		db_manager.DetachDatabase(context, bind_data.db_name, OnEntryNotFound::THROW_EXCEPTION);
		if (fs.FileExists(path)) {
			fs.MoveFile(path, backup_path);
		}
		fs.MoveFile(tmp_path, path);
		fs.TryRemoveFile(tmp_path + ".wal");
		fs.TryRemoveFile(wal_path);
		fs.TryRemoveFile(checkpoint_wal_path);
		fs.TryRemoveFile(recovery_wal_path);
		AttachFileDatabase(context, db_manager, bind_data.db_name, path);
	} catch (const std::exception &ex) {
		error = ErrorData(ex);
	}

	if (error.HasError()) {
		if (fs.FileExists(path) && fs.FileExists(backup_path)) {
			TryRemoveDBFiles(fs, path);
		}
		if (!fs.FileExists(path) && fs.FileExists(backup_path)) {
			fs.MoveFile(backup_path, path);
		}
		TryRemoveDBFiles(fs, tmp_path);
		if (!db_manager.GetDatabase(bind_data.db_name) && fs.FileExists(path)) {
			try {
				AttachFileDatabase(context, db_manager, bind_data.db_name, path);
			} catch (...) {
			}
		}
		RestoreSearchPath(search_path, previous_paths);
		if (created_fallback) {
			TryDetach(context, db_manager, fallback);
		}
		error.Throw();
	}

	try {
		TryRemoveDBFiles(fs, backup_path);
	} catch (...) {
	}
	RestoreSearchPath(search_path, previous_paths);
	if (created_fallback) {
		TryDetach(context, db_manager, fallback);
	}
}

void VacuumFullFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction vacuum_full("vacuum_full", {}, VacuumFullFunctionInternal, VacuumFullBind);
	set.AddFunction(vacuum_full);
}

} // namespace duckdb
