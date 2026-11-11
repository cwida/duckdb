//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/comment_on_column_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {
class CatalogEntryRetriever;
class ClientContext;
class CatalogEntry;

struct SetColumnCommentInfo : public AlterInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::COMMENT_ON_COLUMN_INFO;

public:
	SetColumnCommentInfo();
	SetColumnCommentInfo(QualifiedName qualified_name, Identifier column_name, Value comment_value,
	                     OnEntryNotFound if_not_found);
	//! Compatibility overload for callers that build the qualification from separate parts (e.g. the legacy
	//! parser extension); the qualification is stored as a three-component path.
	SetColumnCommentInfo(Identifier catalog, Identifier schema, Identifier name, Identifier column_name,
	                     Value comment_value, OnEntryNotFound if_not_found);

	//! The resolved Catalog Type
	CatalogType catalog_entry_type;

	//! name of the column to comment on
	Identifier column_name;
	//! The comment, can be NULL or a string
	Value comment_value;

public:
	optional_ptr<CatalogEntry> TryResolveCatalogEntry(CatalogEntryRetriever &retriever);
	unique_ptr<AlterInfo> Copy() const override;
	CatalogType GetCatalogType() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
