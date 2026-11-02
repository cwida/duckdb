//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_schema_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

enum class AlterSchemaType : uint8_t { INVALID = 0, SET_SCHEMA_OPTIONS = 1, RESET_SCHEMA_OPTIONS = 2 };

//! The qualified name uses the same layout as CreateSchemaInfo: [catalog, parent_schemas..., schema, <empty name>]
struct AlterSchemaInfo : public AlterInfo {
public:
	AlterSchemaInfo(AlterSchemaType alter_schema_type, const AlterEntryData &data);
	~AlterSchemaInfo() override;

	AlterSchemaType alter_schema_type;

public:
	//! The name of the schema being altered
	const Identifier &SchemaName() const;
	//! The catalog the schema lives in (empty if the path carries no catalog component)
	const Identifier &SchemaCatalog() const;
	//! The full schema path (parent schemas followed by the altered schema), excluding the catalog
	vector<Identifier> SchemaPath() const;

	CatalogType GetCatalogType() const override;

	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	explicit AlterSchemaInfo(AlterSchemaType alter_schema_type);

	void Serialize(Serializer &serializer) const override;
	string QualifiedSchemaToString() const;
};

//===--------------------------------------------------------------------===//
// SetSchemaOptionsInfo
//===--------------------------------------------------------------------===//
struct SetSchemaOptionsInfo : public AlterSchemaInfo {
	SetSchemaOptionsInfo(const AlterEntryData &data, case_insensitive_map_t<unique_ptr<ParsedExpression>> options);
	~SetSchemaOptionsInfo() override;

	case_insensitive_map_t<unique_ptr<ParsedExpression>> options;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterSchemaInfo> Deserialize(Deserializer &deserializer);

private:
	SetSchemaOptionsInfo();
};

//===--------------------------------------------------------------------===//
// ResetSchemaOptionsInfo
//===--------------------------------------------------------------------===//
struct ResetSchemaOptionsInfo : public AlterSchemaInfo {
	ResetSchemaOptionsInfo(const AlterEntryData &data, identifier_set_t options);
	~ResetSchemaOptionsInfo() override;

	identifier_set_t options;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterSchemaInfo> Deserialize(Deserializer &deserializer);

private:
	ResetSchemaOptionsInfo();
};

} // namespace duckdb
