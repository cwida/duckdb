//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/constraints/foreign_key_constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class ForeignKeyConstraint : public Constraint {
public:
	static constexpr const ConstraintType TYPE = ConstraintType::FOREIGN_KEY;

public:
	DUCKDB_API ForeignKeyConstraint(vector<Identifier> pk_columns, vector<Identifier> fk_columns, ForeignKeyInfo info);

	//! The set of main key table's columns
	vector<Identifier> pk_columns;
	//! The set of foreign key table's columns
	vector<Identifier> fk_columns;
	ForeignKeyInfo info;
	//! The referenced table, including its schema path
	QualifiedName referenced_table;

public:
	DUCKDB_API string ToString() const override;

	DUCKDB_API unique_ptr<Constraint> Copy() const override;

	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<Constraint> Deserialize(Deserializer &deserializer);

private:
	ForeignKeyConstraint();
};

} // namespace duckdb
