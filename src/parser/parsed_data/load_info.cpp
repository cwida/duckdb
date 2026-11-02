#include "duckdb/parser/parsed_data/load_info.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

vector<LogicalType> LoadInfo::GetResultTypes(LoadType load_type) {
	if (load_type == LoadType::CREATE_REPOSITORY) {
		return {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)};
	}
	return {LogicalType::BOOLEAN};
}

vector<Identifier> LoadInfo::GetResultNames(LoadType load_type) {
	if (load_type == LoadType::CREATE_REPOSITORY) {
		return {Identifier("repository_name"), Identifier("prefix"), Identifier("key_fingerprints")};
	}
	return {Identifier("Success")};
}

unique_ptr<LoadInfo> LoadInfo::Copy() const {
	auto result = make_uniq<LoadInfo>();
	result->filename = filename;
	result->repository = repository;
	result->load_type = load_type;
	result->load_after_install = load_after_install;
	result->repo_is_alias = repo_is_alias;
	result->version = version;
	result->alias = alias;
	result->repository_url = repository_url;
	result->public_keys = public_keys;
	result->on_conflict = on_conflict;
	result->missing_ok = missing_ok;
	return result;
}

static string LoadInfoToString(LoadType load_type, bool load_after_install) {
	string suffix = load_after_install ? " AND LOAD" : "";
	switch (load_type) {
	case LoadType::LOAD:
	case LoadType::LOAD_AS:
		return "LOAD";
	case LoadType::INSTALL:
		return "INSTALL" + suffix;
	case LoadType::FORCE_INSTALL:
		return "FORCE INSTALL" + suffix;
	default:
		throw InternalException("ToString for LoadType with type: %s not implemented", EnumUtil::ToString(load_type));
	}
}

//! Serialize the CREATE/DROP EXTENSION REPOSITORY statements
static string RepositoryToString(const LoadInfo &info) {
	string result;
	if (info.load_type == LoadType::CREATE_REPOSITORY) {
		result = "CREATE ";
		if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
			result += "OR REPLACE ";
		}
		result += "EXTENSION REPOSITORY ";
		if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			result += "IF NOT EXISTS ";
		}
		result += SQLIdentifier(info.repository);
		result += " WITH PREFIX " + SQLString(info.repository_url);
		if (!info.public_keys.empty()) {
			result += info.public_keys.size() > 1 ? " USING PUBLIC KEYS " : " USING PUBLIC KEY ";
			for (idx_t i = 0; i < info.public_keys.size(); i++) {
				result += (i == 0 ? "" : ", ") + SQLString(info.public_keys[i]);
			}
		}
	} else {
		result = "DROP EXTENSION REPOSITORY ";
		if (info.missing_ok) {
			result += "IF EXISTS ";
		}
		result += SQLIdentifier(info.repository);
	}
	return result + ";";
}

string LoadInfo::ToString() const {
	if (load_type == LoadType::CREATE_REPOSITORY || load_type == LoadType::DROP_REPOSITORY) {
		return RepositoryToString(*this);
	}
	string result = "";
	result += LoadInfoToString(load_type, load_after_install);
	result += " " + SQLString(filename);
	if (!repository.empty()) {
		if (repo_is_alias) {
			result += " FROM " + SQLIdentifier(repository);
		} else {
			result += " FROM " + SQLString(repository);
		}
	}
	if (!alias.empty()) {
		result += " AS " + SQLIdentifier(alias);
	}

	result += ";";
	return result;
}

} // namespace duckdb
