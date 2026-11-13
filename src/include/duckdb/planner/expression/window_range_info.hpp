//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/window_range_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/expression/window_expression.hpp"

namespace duckdb {
class Expression;

//! A coercion inserted while binding a RANGE boundary, without executable bind data.
struct WindowRangeCast {
	LogicalType source_type;
	LogicalType target_type;
	bool try_cast;
	bool default_cast;

	void Serialize(Serializer &serializer) const;
	static WindowRangeCast Deserialize(Deserializer &deserializer);

	//! Records the casts the binder wrapped around an operand of the range arithmetic. The operand is the expression
	//! still in place, or a copy of a constant operand the function binder may have folded - the chain then ends in
	//! that constant
	static bool Capture(const Expression &expression, const Expression &operand, vector<WindowRangeCast> &casts);
	static optional_ptr<const Expression> Match(const Expression &expression, const vector<WindowRangeCast> &casts);
};

//! Identifies the binder-produced call and its live operands; it owns no column bindings or expressions.
struct WindowRangeBoundary {
	WindowBoundary boundary = WindowBoundary::INVALID;
	OrderType direction = OrderType::INVALID;
	QualifiedName function_name;
	vector<LogicalType> arguments;
	LogicalType return_type;
	LogicalType order_type;
	LogicalType offset_type;
	vector<WindowRangeCast> order_casts;
	vector<WindowRangeCast> result_casts;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<WindowRangeBoundary> Deserialize(Deserializer &deserializer);

	static unique_ptr<WindowRangeBoundary> Capture(const Expression &expression, const Expression &order,
	                                               const Expression &offset);
	optional_ptr<const Expression> Match(const Expression &expression, const Expression &order) const;
};
} // namespace duckdb
