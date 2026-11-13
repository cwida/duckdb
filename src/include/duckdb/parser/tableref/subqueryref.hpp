//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/subqueryref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {
//! Represents a subquery
class SubqueryRef : public TableRef {
public:
	static constexpr const TableReferenceType TYPE = TableReferenceType::SUBQUERY;

private:
	SubqueryRef();

public:
	DUCKDB_API explicit SubqueryRef(unique_ptr<SelectStatement> subquery, Identifier alias = Identifier());

	//! The subquery
	unique_ptr<SelectStatement> subquery;
	//! Whether repeated column names must be renamed - set by rewrites that build a STRUCT out of
	//! the columns of this subquery, which cannot hold the same name twice
	bool deduplicate_column_names = false;

public:
	string ToString() const override;
	bool Equals(const TableRef &other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Deserializes a blob back into a SubqueryRef
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableRef> Deserialize(Deserializer &source);
};
} // namespace duckdb
