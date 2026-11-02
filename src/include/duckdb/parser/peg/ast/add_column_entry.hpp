#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include "duckdb/common/identifier.hpp"
namespace duckdb {

struct AddColumnEntry {
	LogicalType type;
	vector<Identifier> column_path;
	unique_ptr<ParsedExpression> default_value;
	//! Constraints applied via extra ALTER statements after the column is added
	AddColumnConstraints add_column_constraints;
	CompressionType compression_type = CompressionType::COMPRESSION_AUTO;
};

} // namespace duckdb
