//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/type_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

class TypeExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::TYPE;

	TypeExpression(QualifiedName qualified_name, vector<unique_ptr<ParsedExpression>> children);
	TypeExpression(Identifier type_name, vector<unique_ptr<ParsedExpression>> children);
	TypeExpression(const string &type_name, vector<unique_ptr<ParsedExpression>> children);

public:
	//! The type expression that binds back to `type`. The inverse of binding a TypeExpression: every
	//! parameterised built-in is written out with its parameters, and a type that carries an alias (a
	//! user-defined type) is named by that alias. Throws for types that have no SQL spelling.
	DUCKDB_API static unique_ptr<TypeExpression> FromLogicalType(const LogicalType &type);
	//! Whether the type ID has a SQL spelling
	DUCKDB_API static bool IsSQLType(LogicalTypeId id);
	//! Whether the complete type can be written as a SQL type expression
	DUCKDB_API static bool CanRepresent(const LogicalType &type);

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
	const Identifier &GetTypeName() const {
		return qualified_name.Name();
	}
	const Identifier &GetSchema() const {
		return qualified_name.Schema();
	}
	void SetSchema(Identifier new_schema) {
		qualified_name = QualifiedName(qualified_name.Catalog(), std::move(new_schema), qualified_name.Name());
	}
	const Identifier &GetCatalog() const {
		return qualified_name.Catalog();
	}
	void SetCatalog(Identifier new_catalog) {
		qualified_name = qualified_name.WithCatalog(std::move(new_catalog));
	}
	const vector<unique_ptr<ParsedExpression>> &GetChildren() const {
		return children;
	}
	vector<unique_ptr<ParsedExpression>> &GetChildren() {
		return children;
	}

public:
	string ToString() const override;

	unique_ptr<ParsedExpression> Copy() const override;

	bool Equals(const ParsedExpression &other) const override;
	hash_t Hash() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

	void Verify() const override;

private:
	TypeExpression();

	//! Qualified name of the type (catalog.schema.name)
	QualifiedName qualified_name;

	//! Children of the type expression (e.g. type parameters)
	vector<unique_ptr<ParsedExpression>> children;
};

} // namespace duckdb
