#pragma once

#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct ColumnConstraintTypeInfo {
	bool is_primary_key = false;
	ConstraintType type = ConstraintType::INVALID;
	ConstraintCheckMode check_mode = ConstraintCheckMode::DEFAULT;
};

struct ColumnConstraintEntry {
	string constraint_name;
	ColumnConstraintTypeInfo constraint_type_info;
	unique_ptr<ParsedExpression> expression;
	unique_ptr<Constraint> constraint;
	CompressionType compression_type;

	ColumnConstraintEntry() : compression_type(CompressionType::COMPRESSION_AUTO) {
	}
};

} // namespace duckdb
