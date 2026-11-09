//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_load_options.hpp"

namespace duckdb {
class ErrorData;

class ExtensionInfo {
public:
	ExtensionInfo();

	string orig_ext_name;
	string alias;
	mutex lock;
	atomic<bool> is_loaded;
	unique_ptr<ExtensionInstallInfo> install_info;
	unique_ptr<ExtensionLoadedInfo> load_info;
};

class ExtensionActiveLoad {
public:
	ExtensionActiveLoad(DatabaseInstance &db, ExtensionInfo &info, Identifier extension_name_p, Identifier alias_p)
	    : db(db), load_lock(info.lock), info(info), extension_name(std::move(extension_name_p)),
	      alias(std::move(alias_p)) {};
	ExtensionActiveLoad(DatabaseInstance &db, shared_ptr<ExtensionInfo> info_p, Identifier extension_name_p,
	                    Identifier alias_p)
	    : db(db), info_owner(std::move(info_p)), load_lock(info_owner->lock), info(*info_owner),
	      extension_name(std::move(extension_name_p)), alias(std::move(alias_p)) {};

	~ExtensionActiveLoad() = default;

	DatabaseInstance &db;
	//! Keeps the info alive when the extension is removed from the manager during the load
	shared_ptr<ExtensionInfo> info_owner;
	unique_lock<mutex> load_lock;
	ExtensionInfo &info;
	Identifier extension_name;
	Identifier alias;

public:
	void FinishLoad(ExtensionInstallInfo &install_info);
	void LoadFail(const ErrorData &error);
};

class ExtensionManager {
public:
	explicit ExtensionManager(DatabaseInstance &db);

	DUCKDB_API bool ExtensionIsLoaded(const string &name);
	DUCKDB_API vector<string> GetExtensions();
	DUCKDB_API shared_ptr<ExtensionInfo> GetExtensionInfo(const string &name);
	DUCKDB_API unique_ptr<ExtensionActiveLoad> BeginLoad(const ExtensionLoadOptions &options);

	DUCKDB_API static ExtensionManager &Get(DatabaseInstance &db);
	DUCKDB_API static ExtensionManager &Get(ClientContext &context);

	void RemoveExtensionInfo(const string &name);

private:
	DatabaseInstance &db;
	mutex lock;
	identifier_map_t<shared_ptr<ExtensionInfo>> loaded_extensions_info;
	string extension_load_prefix;
};

} // namespace duckdb
