//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/at_clause.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! The AT clause specifies which version of a table to read
class AtClause {
public:
	AtClause(Identifier unit, unique_ptr<ParsedExpression> expr);

public:
	const Identifier &Unit() {
		return unit;
	}
	const unique_ptr<ParsedExpression> &Expression() const {
		return expr;
	}
	unique_ptr<ParsedExpression> &ExpressionMutable() {
		return expr;
	}

	string ToString() const;
	bool Equals(const AtClause &other_p) const;
	unique_ptr<AtClause> Copy() const;
	void Serialize(Serializer &serializer) const;
	static unique_ptr<AtClause> Deserialize(Deserializer &source);

	static bool Equals(optional_ptr<AtClause> lhs, optional_ptr<AtClause> rhs);

private:
	//! The unit (e.g. TIMESTAMP or VERSION)
	Identifier unit;
	//! The expression that determines which value of the unit we want to read
	unique_ptr<ParsedExpression> expr;
};

} // namespace duckdb
