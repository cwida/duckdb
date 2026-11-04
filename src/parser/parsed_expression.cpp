#include "duckdb/main/client_context.hpp"

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/parser/expression_util.hpp"

namespace duckdb {

bool ParsedExpression::IsAggregate() const {
	bool is_aggregate = false;
	ParsedExpressionIterator::EnumerateChildren(
	    *this, [&](const ParsedExpression &child) { is_aggregate |= child.IsAggregate(); });
	return is_aggregate;
}

bool ParsedExpression::IsWindow() const {
	bool is_window = false;
	ParsedExpressionIterator::EnumerateChildren(*this,
	                                            [&](const ParsedExpression &child) { is_window |= child.IsWindow(); });
	return is_window;
}

bool ParsedExpression::IsScalar() const {
	bool is_scalar = true;
	ParsedExpressionIterator::EnumerateChildren(*this, [&](const ParsedExpression &child) {
		if (!child.IsScalar()) {
			is_scalar = false;
		}
	});
	return is_scalar;
}

bool ParsedExpression::HasParameter() const {
	bool has_parameter = false;
	ParsedExpressionIterator::EnumerateChildren(
	    *this, [&](const ParsedExpression &child) { has_parameter |= child.HasParameter(); });
	return has_parameter;
}

bool ParsedExpression::HasSubquery() const {
	bool has_subquery = false;
	ParsedExpressionIterator::EnumerateChildren(
	    *this, [&](const ParsedExpression &child) { has_subquery |= child.HasSubquery(); });
	return has_subquery;
}

bool ParsedExpression::Equals(const unique_ptr<ParsedExpression> &left, const unique_ptr<ParsedExpression> &right) {
	if (left.get() == right.get()) {
		return true;
	}
	if (!left || !right) {
		return false;
	}
	return left->Equals(*right);
}

bool ParsedExpression::ListEquals(const vector<unique_ptr<ParsedExpression>> &left,
                                  const vector<unique_ptr<ParsedExpression>> &right) {
	return ExpressionUtil::ListEquals(left, right);
}

static bool IsComparisonType(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return true;
	default:
		return false;
	}
}

//! Whether an OperatorExpression of the given type can have the given number of children
static bool IsValidOperator(ExpressionType type, idx_t child_count) {
	switch (type) {
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
		return child_count >= 2;
	case ExpressionType::OPERATOR_UNPACK:
	case ExpressionType::OPERATOR_TRY:
	case ExpressionType::OPERATOR_HAS_ERROR:
	case ExpressionType::OPERATOR_NOT:
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		return child_count == 1;
	case ExpressionType::GROUPING_FUNCTION:
		return true;
	case ExpressionType::OPERATOR_COALESCE:
		return child_count >= 1;
	case ExpressionType::ARRAY_EXTRACT:
	case ExpressionType::STRUCT_EXTRACT:
		return child_count == 2;
	case ExpressionType::ARRAY_SLICE:
		return child_count == 3 || child_count == 4;
	case ExpressionType::ARRAY_CONSTRUCTOR:
		return true;
	default:
		return IsComparisonType(type) && child_count == 2;
	}
}

void ParsedExpression::VerifyDeserialized() {
	auto invalid = [&](const string &reason) {
		return SerializationException("Failed to deserialize %s expression: %s",
		                              EnumUtil::ToString(GetExpressionClass()), reason);
	};
	for (auto &child : ChildrenMutable()) {
		if (!child) {
			throw invalid("missing child expression");
		}
	}
	switch (GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
		if (Cast<ColumnRefExpression>().ColumnNames().empty()) {
			throw invalid("no column names");
		}
		break;
	case ExpressionClass::COMPARISON:
		if (!IsComparisonType(GetExpressionType())) {
			throw invalid("invalid comparison type " + EnumUtil::ToString(GetExpressionType()));
		}
		break;
	case ExpressionClass::CONJUNCTION:
		if (GetExpressionType() != ExpressionType::CONJUNCTION_AND &&
		    GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
			throw invalid("invalid conjunction type " + EnumUtil::ToString(GetExpressionType()));
		}
		if (Cast<ConjunctionExpression>().GetChildren().empty()) {
			throw invalid("no children");
		}
		break;
	case ExpressionClass::OPERATOR:
		if (!IsValidOperator(GetExpressionType(), Cast<OperatorExpression>().GetChildren().size())) {
			throw invalid("invalid operator type " + EnumUtil::ToString(GetExpressionType()) + " for " +
			              to_string(Cast<OperatorExpression>().GetChildren().size()) + " children");
		}
		break;
	case ExpressionClass::CASE:
		if (Cast<CaseExpression>().CaseChecks().empty()) {
			throw invalid("no WHEN clauses");
		}
		break;
	case ExpressionClass::FUNCTION:
		if (!Cast<FunctionExpression>().OrderBy()) {
			throw invalid("missing ORDER BY modifier");
		}
		if (Cast<FunctionExpression>().IsOperator() && Cast<FunctionExpression>().Distinct()) {
			throw invalid("an operator cannot be DISTINCT");
		}
		break;
	case ExpressionClass::SUBQUERY: {
		auto &subquery = Cast<SubqueryExpression>();
		if (!subquery.Subquery() || !subquery.Subquery()->node) {
			throw invalid("missing subquery");
		}
		if ((subquery.GetSubqueryType() == SubqueryType::ANY) != (subquery.GetChild() != nullptr)) {
			throw invalid("a child is required for, and only for, ANY subqueries");
		}
		if (subquery.GetSubqueryType() == SubqueryType::INVALID) {
			throw invalid("invalid subquery type");
		}
		break;
	}
	case ExpressionClass::WINDOW: {
		auto &window = Cast<WindowExpression>();
		auto needs_expression = [](WindowBoundary boundary) {
			switch (boundary) {
			case WindowBoundary::EXPR_PRECEDING_ROWS:
			case WindowBoundary::EXPR_PRECEDING_RANGE:
			case WindowBoundary::EXPR_PRECEDING_GROUPS:
			case WindowBoundary::EXPR_FOLLOWING_ROWS:
			case WindowBoundary::EXPR_FOLLOWING_RANGE:
			case WindowBoundary::EXPR_FOLLOWING_GROUPS:
				return true;
			default:
				return false;
			}
		};
		if (window.WindowStart() == WindowBoundary::INVALID ||
		    window.WindowStart() == WindowBoundary::UNBOUNDED_FOLLOWING) {
			throw invalid("invalid frame start boundary");
		}
		if (window.WindowEnd() == WindowBoundary::INVALID) {
			throw invalid("invalid frame end boundary");
		}
		if (needs_expression(window.WindowStart()) && !window.StartExpr()) {
			throw invalid("missing frame start expression");
		}
		if (needs_expression(window.WindowEnd()) && !window.EndExpr()) {
			throw invalid("missing frame end expression");
		}
		break;
	}
	case ExpressionClass::LAMBDA_REF:
		throw invalid("lambda references cannot be deserialized");
	default:
		break;
	}
}

} // namespace duckdb
