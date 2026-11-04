//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/tableref/bound_at_clause.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

//! The AT clause specifies which version of a table to read
class BoundAtClause {
public:
	BoundAtClause(Identifier unit_p, Value value_p) : unit(std::move(unit_p)), val(std::move(value_p)) {
	}

public:
	const Identifier &Unit() const {
		return unit;
	}
	const Value &GetValue() const {
		return val;
	}

	void Serialize(Serializer &serializer) const;
	static unique_ptr<BoundAtClause> Deserialize(Deserializer &deserializer);

private:
	//! The unit (e.g. TIMESTAMP or VERSION)
	Identifier unit;
	//! The value that is associated with the unit (e.g. TIMESTAMP '2020-01-01')
	Value val;
};

} // namespace duckdb
