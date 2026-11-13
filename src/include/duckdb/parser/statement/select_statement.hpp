//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/statement/select_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

class QueryNode;
class Serializer;
class Deserializer;

//! SelectStatement is a typical SELECT clause
class SelectStatement : public SQLStatement {
public:
	static constexpr const StatementType TYPE = StatementType::SELECT_STATEMENT;

public:
	SelectStatement() : SQLStatement(StatementType::SELECT_STATEMENT) {
	}

	//! The main query node
	unique_ptr<QueryNode> node;
	//! Scalar queries that must be evaluated before binding AT clauses
	vector<unique_ptr<SelectStatement>> at_clause_subqueries;

protected:
	SelectStatement(const SelectStatement &other);

public:
	//! Convert the SELECT statement to a string

	DUCKDB_API string ToString() const override;
	//! Create a copy of this SelectStatement
	DUCKDB_API unique_ptr<SQLStatement> Copy() const override;
	//! Whether or not the statements are equivalent
	bool Equals(const SQLStatement &other) const;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<SelectStatement> Deserialize(Deserializer &deserializer);
};
} // namespace duckdb
