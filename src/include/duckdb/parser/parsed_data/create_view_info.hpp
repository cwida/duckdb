//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_view_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include "duckdb/common/enums/view_security_type.hpp"
#include "duckdb/common/identifier.hpp"
namespace duckdb {
class Parser;
class SchemaCatalogEntry;

enum class CreateViewBindingMode { BIND_ON_CREATE, SKIP_BINDING };

struct CreateViewInfo : public CreateInfo {
public:
	CreateViewInfo();
	CreateViewInfo(SchemaCatalogEntry &schema, const Identifier &view_name);
	explicit CreateViewInfo(const QualifiedName &view_name);

public:
	//! View name
	const Identifier &GetViewName() const {
		return qualified_name.Name();
	}
	void SetViewName(Identifier name) {
		qualified_name = qualified_name.WithName(std::move(name));
	}
	//! Aliases of the view
	vector<Identifier> aliases;
	//! Return types
	vector<LogicalType> types;
	//! Names of the query
	vector<Identifier> names;
	//! Comments on columns of the query. Note: vector can be empty when no comments are set
	identifier_map_t<Value> column_comments_map;
	//! The SelectStatement of the view
	unique_ptr<SelectStatement> query;
	//! Whether or not to bind the view on create
	CreateViewBindingMode binding_mode = CreateViewBindingMode::BIND_ON_CREATE;
	//! Whether this is a secure view - secure views act as an optimization barrier
	ViewSecurityType security_type = ViewSecurityType::REGULAR_VIEW;

public:
	unique_ptr<CreateInfo> Copy() const override;

	//! Gets a bound CreateViewInfo object from a SELECT statement and a view name, schema name, etc
	DUCKDB_API static unique_ptr<CreateViewInfo> FromSelect(Parser &parser, unique_ptr<CreateViewInfo> info);
	//! Gets a bound CreateViewInfo object from a CREATE VIEW statement
	DUCKDB_API static unique_ptr<CreateViewInfo> FromCreateView(ClientContext &context, SchemaCatalogEntry &schema,
	                                                            const string &sql);
	//! Parse a SELECT statement from a SQL string
	DUCKDB_API static unique_ptr<SelectStatement> ParseSelect(Parser &parser, const string &sql);

	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	string ToString() const override;

private:
	CreateViewInfo(vector<Identifier> names, vector<Value> comments, identifier_map_t<Value> column_comments);

	vector<Value> GetColumnCommentsList() const;
};

} // namespace duckdb
