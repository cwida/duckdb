//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/catalog/dependency_list.hpp"

namespace duckdb {
struct AlterInfo;

struct CreateInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::CREATE_INFO;

public:
	explicit CreateInfo(CatalogType type, Identifier schema = Identifier::DefaultSchema(),
	                    Identifier catalog_p = Identifier::InvalidCatalog())
	    : ParseInfo(TYPE), type(type), on_conflict(OnCreateConflict::ERROR_ON_CONFLICT), temporary(false),
	      internal(false), qualified_name(std::move(catalog_p), std::move(schema), Identifier()) {
	}
	~CreateInfo() override {
	}

	//! The to-be-created catalog type
	CatalogType type;
	//! What to do on create conflict
	OnCreateConflict on_conflict;
	//! Whether or not the entry is temporary
	bool temporary;
	//! Whether or not the entry is an internal entry
	bool internal;
	//! The name of the extension that registered this entry (empty for core entries)
	Identifier extension_name;
	//! The SQL string of the CREATE statement
	string sql;
	//! The inherent dependencies of the created entry
	LogicalDependencyList dependencies;
	//! User provided comment
	Value comment;
	//! Key-value tags with additional metadata
	InsertionOrderPreservingMap<string> tags;

public:
	DatabaseModificationType GetModifications() const;
	const QualifiedName &GetQualifiedName() const {
		return qualified_name;
	}
	QualifiedName &GetQualifiedNameMutable() {
		return qualified_name;
	}
	void SetQualifiedName(QualifiedName name) {
		qualified_name = std::move(name);
	}
	//! Set the name, keeping the catalog/schema qualification
	void SetName(Identifier name) {
		qualified_name = qualified_name.WithName(std::move(name));
	}
	//! Set the catalog/schema qualification, keeping the name
	void SetQualification(Identifier catalog, Identifier schema) {
		qualified_name = QualifiedName(std::move(catalog), std::move(schema), qualified_name.Name());
	}
	//! Set the schema, keeping the catalog and name
	void SetSchema(Identifier schema) {
		qualified_name = QualifiedName(qualified_name.Catalog(), std::move(schema), qualified_name.Name());
	}
	//! Set the catalog, keeping the schema and name
	void SetCatalog(Identifier catalog) {
		qualified_name = qualified_name.WithCatalog(std::move(catalog));
	}
	//! Renders the qualified name for ToString - the catalog is omitted for temporary entries and the default schema is
	//! hidden
	DUCKDB_API string QualifiedNameToString() const;
	//! Drop the catalog component from the qualified name, keeping the (possibly nested) schema path. Use this before
	//! rendering an entry that lives in a catalog: its catalog is implied by where the statement is run.
	DUCKDB_API void StripCatalogQualification();

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	virtual unique_ptr<CreateInfo> Copy() const = 0;

	DUCKDB_API void CopyProperties(CreateInfo &other) const;
	//! Generates an alter statement from the create statement - used for OnCreateConflict::ALTER_ON_CONFLICT
	DUCKDB_API virtual unique_ptr<AlterInfo> GetAlterInfo() const;
	//! Returns a string like "CREATE (OR REPLACE) (TEMPORARY) <entry> (IF NOT EXISTS) " for TABLE/VIEW/TYPE/MACRO
	DUCKDB_API string GetCreatePrefix(const string &entry) const;

	virtual string ToString() const {
		throw NotImplementedException("ToString not supported for this type of CreateInfo: '%s'",
		                              EnumUtil::ToString(info_type));
	}

protected:
	//! Qualified name of the created entry (catalog.schema.name)
	QualifiedName qualified_name;
};

} // namespace duckdb
