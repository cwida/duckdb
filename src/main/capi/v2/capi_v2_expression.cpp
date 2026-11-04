#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb::capiv2 {

// The public enum mirrors the engine's ExpressionType numerically for the values it exposes, so that the mapping
// below is a plain cast. Any renumbering of an exposed value is caught here instead of surfacing as a wrong type.
#define CV2_ASSERT_EXPRESSION_TYPE(NAME)                                                                               \
	static_assert(static_cast<int>(ExpressionType::NAME) == DUCKDB_V2_EXPRESSION_TYPE_##NAME,                          \
	              "EXPRESSION_TYPE numeric drift: " #NAME)

CV2_ASSERT_EXPRESSION_TYPE(INVALID);
CV2_ASSERT_EXPRESSION_TYPE(OPERATOR_CAST);
CV2_ASSERT_EXPRESSION_TYPE(OPERATOR_NOT);
CV2_ASSERT_EXPRESSION_TYPE(OPERATOR_IS_NULL);
CV2_ASSERT_EXPRESSION_TYPE(OPERATOR_IS_NOT_NULL);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_EQUAL);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_NOTEQUAL);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_LESSTHAN);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_GREATERTHAN);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_LESSTHANOREQUALTO);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_GREATERTHANOREQUALTO);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_IN);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_NOT_IN);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_DISTINCT_FROM);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_BETWEEN);
CV2_ASSERT_EXPRESSION_TYPE(COMPARE_NOT_DISTINCT_FROM);
CV2_ASSERT_EXPRESSION_TYPE(CONJUNCTION_AND);
CV2_ASSERT_EXPRESSION_TYPE(CONJUNCTION_OR);
CV2_ASSERT_EXPRESSION_TYPE(VALUE_CONSTANT);
CV2_ASSERT_EXPRESSION_TYPE(VALUE_PARAMETER);
CV2_ASSERT_EXPRESSION_TYPE(BOUND_FUNCTION);
CV2_ASSERT_EXPRESSION_TYPE(CASE_EXPR);
CV2_ASSERT_EXPRESSION_TYPE(OPERATOR_COALESCE);
CV2_ASSERT_EXPRESSION_TYPE(BOUND_COLUMN_REF);

#undef CV2_ASSERT_EXPRESSION_TYPE

//! Maps an expression type onto the public enum: the exposed subset passes through, everything else is INVALID.
static auto CV2ExpressionGetType(const Expression &expression) -> DUCKDB_V2_EXPRESSION_TYPE {
	switch (expression.GetExpressionType()) {
	case ExpressionType::OPERATOR_CAST:
	case ExpressionType::OPERATOR_NOT:
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_BETWEEN:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
	case ExpressionType::CONJUNCTION_AND:
	case ExpressionType::CONJUNCTION_OR:
	case ExpressionType::VALUE_CONSTANT:
	case ExpressionType::VALUE_PARAMETER:
	case ExpressionType::BOUND_FUNCTION:
	case ExpressionType::CASE_EXPR:
	case ExpressionType::OPERATOR_COALESCE:
	case ExpressionType::BOUND_COLUMN_REF:
		return static_cast<DUCKDB_V2_EXPRESSION_TYPE>(expression.GetExpressionType());
	default:
		return DUCKDB_V2_EXPRESSION_TYPE_INVALID;
	}
}

// Children are walked with the engine's own iterator, which covers every expression class, so the walk is total
// even over nodes the public enum does not model. Fan-outs are small, so counting on every call is fine.
static auto CV2ExpressionChildCount(Expression &expression) -> idx_t {
	idx_t count = 0;
	ExpressionIterator::EnumerateChildren(expression, [&](Expression &) { count++; });
	return count;
}

static auto CV2ExpressionChild(Expression &expression, idx_t index) -> Expression & {
	optional_ptr<Expression> result;
	idx_t current = 0;
	ExpressionIterator::EnumerateChildren(expression, [&](Expression &child) {
		if (current++ == index) {
			result = &child;
		}
	});
	if (!result) {
		throw InvalidInputException("Index out of bounds in duckdb_v2_expression_get_child");
	}
	return *result;
}

static auto CV2ExpressionRequireType(const Expression &expression, ExpressionType type, const char *function) -> void {
	if (expression.GetExpressionType() != type) {
		throw InvalidInputException("%s: expression is not of type %s", function, ExpressionTypeToString(type));
	}
}

//! The bound scalar function behind a function node, for the accessors that only apply to function calls.
static auto CV2ExpressionRequireFunction(const Expression &expression, const char *function)
    -> const BoundScalarFunction & {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InvalidInputException("%s: expression is not a function call", function);
	}
	return expression.Cast<BoundFunctionExpression>().Function();
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_expression_get_type(duckdb_v2_expression_handle expression, DUCKDB_V2_EXPRESSION_TYPE *type,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(type);
	return WithErrorHandler(err, [&]() { *type = CV2ExpressionGetType(*Convert(expression)); });
}

DUCKDB_V2_ERROR duckdb_v2_expression_get_return_type(duckdb_v2_expression_handle expression,
                                                     duckdb_v2_logical_type_handle *type,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() { *type = Convert(Convert(expression)->GetReturnType()); });
}

DUCKDB_V2_ERROR duckdb_v2_expression_get_child_count(duckdb_v2_expression_handle expression, idx_t *count,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = CV2ExpressionChildCount(*Convert(expression)); });
}

DUCKDB_V2_ERROR duckdb_v2_expression_get_child(duckdb_v2_expression_handle expression, idx_t index,
                                               duckdb_v2_expression_handle *child, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(child);
	*child = nullptr;
	return WithErrorHandler(err, [&]() { *child = Convert(&CV2ExpressionChild(*Convert(expression), index)); });
}

DUCKDB_V2_ERROR duckdb_v2_expression_constant_get_value(duckdb_v2_expression_handle expression,
                                                        duckdb_v2_value_handle *value,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(err, [&]() {
		const auto &node = *Convert(expression);
		CV2ExpressionRequireType(node, duckdb::ExpressionType::VALUE_CONSTANT,
		                         "duckdb_v2_expression_constant_get_value");
		*value = Convert(new duckdb::Value(node.Cast<duckdb::BoundConstantExpression>().GetValue()));
	});
}

DUCKDB_V2_ERROR duckdb_v2_expression_column_ref_get_index(duckdb_v2_expression_handle expression, idx_t *index,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(index);
	return WithErrorHandler(err, [&]() {
		const auto &node = *Convert(expression);
		CV2ExpressionRequireType(node, duckdb::ExpressionType::BOUND_COLUMN_REF,
		                         "duckdb_v2_expression_column_ref_get_index");
		*index = node.Cast<duckdb::BoundColumnRefExpression>().Binding().column_index.GetIndex();
	});
}

DUCKDB_V2_ERROR duckdb_v2_expression_function_get_name(duckdb_v2_expression_handle expression,
                                                       duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(name);
	*name = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() {
		const auto &function =
		    CV2ExpressionRequireFunction(*Convert(expression), "duckdb_v2_expression_function_get_name");
		// Borrowed from the bound function's own name, which lives as long as the node.
		*name = Convert(function.GetName());
	});
}

DUCKDB_V2_ERROR duckdb_v2_expression_function_get_qname(duckdb_v2_expression_handle expression,
                                                        duckdb_v2_qname_handle *name,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(name);
	*name = nullptr;
	return WithErrorHandler(err, [&]() {
		const auto &function =
		    CV2ExpressionRequireFunction(*Convert(expression), "duckdb_v2_expression_function_get_qname");
		// Only the qualifiers the binder actually resolved: a function bound outside the catalog has none.
		duckdb::vector<duckdb::Identifier> path;
		if (!function.GetCatalogName().empty()) {
			path.push_back(function.GetCatalogName());
		}
		if (!function.GetSchemaName().empty()) {
			path.push_back(function.GetSchemaName());
		}
		*name = Convert(new duckdb::QualifiedName(std::move(path), function.GetName()));
	});
}

DUCKDB_V2_ERROR duckdb_v2_expression_cast_get_mode(duckdb_v2_expression_handle expression, DUCKDB_V2_CAST_MODE *mode,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(expression);
	DUCKDB_CHECK_ARG(mode);
	return WithErrorHandler(err, [&]() {
		const auto &node = *Convert(expression);
		CV2ExpressionRequireType(node, duckdb::ExpressionType::OPERATOR_CAST, "duckdb_v2_expression_cast_get_mode");
		const auto &cast = node.Cast<duckdb::BoundFunctionExpression>();
		*mode = duckdb::BoundCastExpression::IsTryCast(cast) ? DUCKDB_V2_CAST_MODE_TRY : DUCKDB_V2_CAST_MODE_NORMAL;
	});
}
