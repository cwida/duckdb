#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/peg/ast/column_constraint_entry.hpp"

namespace duckdb {

struct ColumnConstraint {
	vector<unique_ptr<Constraint>> constraints;
	vector<ColumnConstraintTypeInfo> constraint_types; // Used to create proper constraints when column index is known
	unique_ptr<ParsedExpression> default_value;
};

} // namespace duckdb
