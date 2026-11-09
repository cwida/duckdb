//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/drop_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/secret/secret.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

enum class ExtraDropInfoType : uint8_t {
	INVALID = 0,

	SECRET_INFO = 1,
	TRIGGER_INFO = 2
};

struct ExtraDropInfo {
	explicit ExtraDropInfo(ExtraDropInfoType info_type) : info_type(info_type) {
	}
	virtual ~ExtraDropInfo() {
	}

	ExtraDropInfoType info_type;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
	virtual unique_ptr<ExtraDropInfo> Copy() const = 0;

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<ExtraDropInfo> Deserialize(Deserializer &deserializer);
};

struct ExtraDropTriggerInfo : public ExtraDropInfo {
	ExtraDropTriggerInfo();
	ExtraDropTriggerInfo(const ExtraDropTriggerInfo &info);

	//! Table the trigger is on
	unique_ptr<TableRef> base_table;

public:
	unique_ptr<ExtraDropInfo> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ExtraDropInfo> Deserialize(Deserializer &deserializer);
};

struct ExtraDropSecretInfo : public ExtraDropInfo {
	ExtraDropSecretInfo();
	ExtraDropSecretInfo(const ExtraDropSecretInfo &info);

	//! Secret Persistence
	SecretPersistType persist_mode;
	//! (optional) the name of the storage to drop from
	string secret_storage;

public:
	unique_ptr<ExtraDropInfo> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ExtraDropInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
