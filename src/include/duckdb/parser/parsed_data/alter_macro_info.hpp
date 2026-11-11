//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_macro_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/function/macro_function.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {

enum class AlterMacroType : uint8_t { INVALID = 0, REMOVE_OVERLOAD = 1, ADD_OVERLOAD = 2 };

//===--------------------------------------------------------------------===//
// Alter Macro
//===--------------------------------------------------------------------===//
struct AlterMacroInfo : public AlterInfo {
public:
	AlterMacroInfo(AlterMacroType alter_macro_type, const AlterEntryData &data, CatalogType entry_catalog_type);
	~AlterMacroInfo() override;

	AlterMacroType alter_macro_type;
	//! MACRO_ENTRY or TABLE_MACRO_ENTRY
	CatalogType entry_catalog_type;

public:
	CatalogType GetCatalogType() const override;
	//! DROP MACRO also names a table macro
	vector<CatalogType> GetEntryLookupTypes() const override;
	void SetResolvedEntryType(CatalogType type) override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	explicit AlterMacroInfo(AlterMacroType alter_macro_type);
};

//===--------------------------------------------------------------------===//
// RemoveMacroOverloadInfo
//===--------------------------------------------------------------------===//
struct RemoveMacroOverloadInfo : public AlterMacroInfo {
	RemoveMacroOverloadInfo(const AlterEntryData &data, CatalogType entry_catalog_type, vector<LogicalType> signature);
	~RemoveMacroOverloadInfo() override;

	//! The parameter types identifying the overload to remove (ANY names an untyped slot)
	vector<LogicalType> signature;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	vector<reference<LogicalType>> GetTypesToBind() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterMacroInfo> Deserialize(Deserializer &deserializer);

private:
	RemoveMacroOverloadInfo();
};

//===--------------------------------------------------------------------===//
// AddMacroOverloadInfo
//===--------------------------------------------------------------------===//
struct AddMacroOverloadInfo : public AlterMacroInfo {
	AddMacroOverloadInfo(const AlterEntryData &data, unique_ptr<MacroFunction> member,
	                     OnCreateConflict member_on_conflict);
	~AddMacroOverloadInfo() override;

	//! The member to add to the existing overload set
	unique_ptr<MacroFunction> member;
	//! How the member treats an existing overload it conflicts with
	OnCreateConflict member_on_conflict;
	//! Documentation published for the member, in-memory like all function descriptions
	vector<FunctionDescription> descriptions;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterMacroInfo> Deserialize(Deserializer &deserializer);

private:
	AddMacroOverloadInfo();
};

} // namespace duckdb
