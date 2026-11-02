//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/expression_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Predicate Expression Operation Types
//===--------------------------------------------------------------------===//
enum class ExpressionType : uint8_t {
	INVALID = 0,

	// explicitly cast left as right (right is integer in ValueType enum)
	OPERATOR_CAST = 12,
	// logical not operator
	OPERATOR_NOT = 13,
	// is null operator
	OPERATOR_IS_NULL = 14,
	// is not null operator
	OPERATOR_IS_NOT_NULL = 15,
	// unpack operator
	OPERATOR_UNPACK = 16,

	// -----------------------------
	// Comparison Operators
	// -----------------------------
	// equal operator between left and right
	COMPARE_EQUAL = 25,
	// compare initial boundary
	COMPARE_BOUNDARY_START = COMPARE_EQUAL,
	// inequal operator between left and right
	COMPARE_NOTEQUAL = 26,
	// less than operator between left and right
	COMPARE_LESSTHAN = 27,
	// greater than operator between left and right
	COMPARE_GREATERTHAN = 28,
	// less than equal operator between left and right
	COMPARE_LESSTHANOREQUALTO = 29,
	// greater than equal operator between left and right
	COMPARE_GREATERTHANOREQUALTO = 30,
	// IN operator [left IN (right1, right2, ...)]
	COMPARE_IN = 35,
	// NOT IN operator [left NOT IN (right1, right2, ...)]
	COMPARE_NOT_IN = 36,
	// IS DISTINCT FROM operator
	COMPARE_DISTINCT_FROM = 37,

	COMPARE_BETWEEN = 38,
	COMPARE_NOT_BETWEEN = 39,
	// IS NOT DISTINCT FROM operator
	COMPARE_NOT_DISTINCT_FROM = 40,
	// compare final boundary
	COMPARE_BOUNDARY_END = COMPARE_NOT_DISTINCT_FROM,

	// -----------------------------
	// Conjunction Operators
	// -----------------------------
	CONJUNCTION_AND = 50,
	CONJUNCTION_OR = 51,

	// -----------------------------
	// Values
	// -----------------------------
	VALUE_CONSTANT = 75,
	VALUE_PARAMETER = 76,
	VALUE_TUPLE = 77,
	VALUE_TUPLE_ADDRESS = 78,
	VALUE_NULL = 79,
	VALUE_VECTOR = 80,
	VALUE_SCALAR = 81,
	VALUE_DEFAULT = 82,

	// -----------------------------
	// Aggregates
	// -----------------------------
	AGGREGATE = 100,
	BOUND_AGGREGATE = 101,
	GROUPING_FUNCTION = 102,

	// -----------------------------
	// Window Functions
	// -----------------------------
	WINDOW_AGGREGATE = 110,
	WINDOW_FUNCTION = 111,

	WINDOW_RANK = 120,
	WINDOW_RANK_DENSE = 121,
	WINDOW_NTILE = 122,
	WINDOW_PERCENT_RANK = 123,
	WINDOW_CUME_DIST = 124,
	WINDOW_ROW_NUMBER = 125,

	WINDOW_FIRST_VALUE = 130,
	WINDOW_LAST_VALUE = 131,
	WINDOW_LEAD = 132,
	WINDOW_LAG = 133,
	WINDOW_NTH_VALUE = 134,
	WINDOW_FILL = 135,

	// -----------------------------
	// Functions
	// -----------------------------
	FUNCTION = 140,
	BOUND_FUNCTION = 141,

	// -----------------------------
	// Operators
	// -----------------------------
	CASE_EXPR = 150,
	OPERATOR_NULLIF = 151,
	OPERATOR_COALESCE = 152,
	ARRAY_EXTRACT = 153,
	ARRAY_SLICE = 154,
	STRUCT_EXTRACT = 155,
	ARRAY_CONSTRUCTOR = 156,
	ARROW = 157,
	OPERATOR_TRY = 158,
	OPERATOR_HAS_ERROR = 159,

	// -----------------------------
	// Subquery IN/EXISTS
	// -----------------------------
	SUBQUERY = 175,

	// -----------------------------
	// Parser
	// -----------------------------
	STAR = 200,
	// MATCH_RECOGNIZE
	QUANTIFIER = 210,
	CONCATENATION = 211,
	ALTERNATION = 212,
	ANCHOR = 213,
	TABLE_STAR = 201,
	PLACEHOLDER = 202,
	COLUMN_REF = 203,
	FUNCTION_REF = 204,
	TABLE_REF = 205,
	LAMBDA_REF = 206,
	TYPE = 207,

	// -----------------------------
	// Miscellaneous
	// -----------------------------
	CAST = 225,
	BOUND_REF = 227,
	BOUND_COLUMN_REF = 228,
	BOUND_UNNEST = 229,
	COLLATE = 230,
	LAMBDA = 231,
	POSITIONAL_REFERENCE = 232,
	BOUND_LAMBDA_REF = 233,
	BOUND_EXPANDED = 234
};

//===--------------------------------------------------------------------===//
// Expression Class
//===--------------------------------------------------------------------===//
enum class ExpressionClass : uint8_t {
	INVALID = 0,
	//===--------------------------------------------------------------------===//
	// Parsed Expressions
	//===--------------------------------------------------------------------===//
	AGGREGATE = 1,
	CASE = 2,
	CAST = 3,
	COLUMN_REF = 4,
	COMPARISON = 5,
	CONJUNCTION = 6,
	CONSTANT = 7,
	DEFAULT = 8,
	FUNCTION = 9,
	OPERATOR = 10,
	STAR = 11,
	SUBQUERY = 13,
	WINDOW = 14,
	PARAMETER = 15,
	COLLATE = 16,
	LAMBDA = 17,
	POSITIONAL_REFERENCE = 18,
	BETWEEN = 19,
	LAMBDA_REF = 20,
	TYPE = 21,
	PATTERN = 22,
	//===--------------------------------------------------------------------===//
	// Bound Expressions
	//===--------------------------------------------------------------------===//
	BOUND_AGGREGATE = 25,
	BOUND_CASE = 26,
	LEGACY_BOUND_CAST = 27,
	BOUND_COLUMN_REF = 28,
	LEGACY_BOUND_COMPARISON = 29,
	BOUND_CONJUNCTION = 30,
	BOUND_CONSTANT = 31,
	BOUND_DEFAULT = 32,
	BOUND_FUNCTION = 33,
	BOUND_OPERATOR = 34,
	BOUND_PARAMETER = 35,
	BOUND_REF = 36,
	BOUND_SUBQUERY = 37,
	BOUND_WINDOW = 38,
	LEGACY_BOUND_BETWEEN = 39,
	BOUND_UNNEST = 40,
	BOUND_LAMBDA = 41,
	BOUND_LAMBDA_REF = 42,
	//===--------------------------------------------------------------------===//
	// Miscellaneous
	//===--------------------------------------------------------------------===//
	BOUND_EXPANDED = 51
};

DUCKDB_API string ExpressionTypeToString(ExpressionType type);
string ExpressionTypeToOperator(ExpressionType type);

// Operator String to ExpressionType (e.g. + => OPERATOR_ADD)
ExpressionType OperatorToExpressionType(const string &op);
//! Negate a comparison expression, turning e.g. = into !=, or < into >=
ExpressionType NegateComparisonExpression(ExpressionType type);
//! Flip a comparison expression, turning e.g. < into >, or = into =
ExpressionType FlipComparisonExpression(ExpressionType type);

DUCKDB_API string ExpressionClassToString(ExpressionClass type);

} // namespace duckdb
