//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/load_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/parse_info.hpp"

#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/identifier.hpp"
namespace duckdb {

enum class LoadType : uint8_t { LOAD, INSTALL, FORCE_INSTALL, LOAD_AS, CREATE_REPOSITORY, DROP_REPOSITORY };

struct LoadInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::LOAD_INFO;

public:
	LoadInfo() : ParseInfo(TYPE) {
	}

	string filename;
	string repository;
	bool repo_is_alias;
	string version;
	LoadType load_type;
	//! Whether to load the extension immediately after installing it (INSTALL AND LOAD)
	bool load_after_install = false;
	Identifier alias;
	//! The url prefix of the repository (CREATE_REPOSITORY only). Empty when none was given: the name is then the host
	//! of the publisher, which names the prefix in its .well-known metadata file
	string repository_url;
	//! The public keys that are trusted to sign the extensions of the repository (CREATE_REPOSITORY only)
	vector<string> public_keys;
	//! What to do when the repository already exists (CREATE_REPOSITORY only)
	OnCreateConflict on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	//! Whether or not to throw if the repository does not exist (DROP_REPOSITORY only)
	bool missing_ok = false;

public:
	//! CREATE EXTENSION REPOSITORY returns the repository it created, everything else returns nothing
	static vector<LogicalType> GetResultTypes(LoadType load_type);
	static vector<Identifier> GetResultNames(LoadType load_type);

	unique_ptr<LoadInfo> Copy() const;
	string ToString() const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
