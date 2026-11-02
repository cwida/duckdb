#pragma once

#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct IsDistinctFromTail {
	ExpressionType comparison_type;
	unique_ptr<ParsedExpression> expression;
};

struct ComparisonExpressionTail {
	ExpressionType comparison_type;
	unique_ptr<ParsedExpression> expression;
};

struct BetweenInLikeOperator {
	bool has_not = false;
	unique_ptr<ParsedExpression> expression;
	vector<unique_ptr<ParsedExpression>> casts;
};

struct ParsedOperator {
	string name;
	bool is_any_all = false;
	bool is_any = true;
};

struct OtherOperatorTail {
	ParsedOperator op;
	unique_ptr<ParsedExpression> expression;
};

enum class ExpressionTailType : uint8_t { IS_TEST, DISTINCT, COMPARISON, OTHER_OPERATOR, CAST };

struct IsExpressionTail {
	ExpressionTailType type = ExpressionTailType::IS_TEST;
	unique_ptr<ParsedExpression> test;
	IsDistinctFromTail distinct;
	ComparisonExpressionTail comparison;
	OtherOperatorTail other;
	unique_ptr<ParsedExpression> cast;
};

struct BinaryExpressionTail {
	string op;
	unique_ptr<ParsedExpression> expression;
	optional_idx query_location;
};

} // namespace duckdb
