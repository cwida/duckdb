//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/drop_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {
struct ExtraDropInfo;

struct DropInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::DROP_INFO;

public:
	DropInfo();
	DropInfo(const DropInfo &info);

	//! The catalog type to drop
	CatalogType type;
	//! Ignore if the entry does not exist instead of failing
	OnEntryNotFound if_not_found = OnEntryNotFound::THROW_EXCEPTION;
	//! Cascade drop (drop all dependents instead of throwing an error if there
	//! are any)
	bool cascade = false;
	//! Allow dropping of internal system entries
	bool allow_drop_internal = false;
	//! Extra info related to this drop
	unique_ptr<ExtraDropInfo> extra_drop_info;

public:
	const QualifiedName &GetQualifiedName() const {
		return qualified_name;
	}
	QualifiedName &GetQualifiedNameMutable() {
		return qualified_name;
	}
	void SetQualifiedName(QualifiedName name) {
		qualified_name = std::move(name);
	}
	void SetQualifiedName(Identifier catalog, Identifier schema, Identifier name) {
		qualified_name = QualifiedName(std::move(catalog), std::move(schema), std::move(name));
	}
	void SetName(Identifier name) {
		qualified_name = qualified_name.WithName(std::move(name));
	}
	void SetSchema(Identifier schema) {
		qualified_name = QualifiedName(qualified_name.Catalog(), std::move(schema), qualified_name.Name());
	}
	void SetCatalog(Identifier catalog) {
		qualified_name = qualified_name.WithCatalog(std::move(catalog));
	}

public:
	virtual unique_ptr<DropInfo> Copy() const;
	DatabaseModificationType GetModifications() const;
	string ToString() const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);

private:
	//! Qualified name of the entry to drop (catalog.schema.name)
	QualifiedName qualified_name;
};

} // namespace duckdb
