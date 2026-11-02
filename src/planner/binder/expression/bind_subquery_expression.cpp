#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/case_operand_binding.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static bool TypeIsTuple(const LogicalType &type) {
	return type.id() == LogicalTypeId::TUPLE;
}

static void ExtractSubqueryChildren(unique_ptr<Expression> &child, vector<unique_ptr<Expression>> &result,
                                    const vector<LogicalType> &types, ExpressionType comparison_type) {
	// two scenarios
	// Single Expression (standard):
	// x IN (...)
	// Multi-Expression/Struct:
	// (a, b) IN (SELECT ...)
	// the latter has an tuple on the LHS that is created by a "ROW" expression
	auto &return_type = child->GetReturnType();
	if (!TypeIsTuple(return_type)) {
		// child is not a tuple struct
		return;
	}
	if (child->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		// not a function
		return;
	}
	auto &function = child->Cast<BoundFunctionExpression>();
	if (function.Function().GetName() != "row") {
		// not "ROW"
		return;
	}
	// we found (a, b, ...) - we can extract all children of this function
	// note that we don't always want to do this
	if (types.size() == 1 && TypeIsTuple(types[0]) && function.GetChildrenMutable().size() != types.size()) {
		// old case: we have an unnamed struct INSIDE the subquery as well
		// i.e. (a, b) IN (SELECT (a, b) ...)
		// unnesting the struct is guaranteed to throw an error - match the structs against each-other instead
		return;
	}
	// For ordered comparisons (<, <=, >, >=), we cannot extract children
	// because row comparison is lexicographic, not element-wise
	// e.g. (0, 0) < (1, 0) is TRUE (first element comparison wins)
	// but if we split into separate conditions: 0 < 1 AND 0 < 0, this becomes FALSE
	// Only comparisons whose row semantics are element-wise can be safely split.
	// Row-valued <> is not element-wise under SQL three-valued logic:
	// e.g. (1, 2) <> (1, NULL) yields NULL, not FALSE.
	if (comparison_type != ExpressionType::COMPARE_EQUAL && comparison_type != ExpressionType::COMPARE_DISTINCT_FROM &&
	    comparison_type != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		// Keep the struct intact for row-valued comparisons with non-element-wise semantics.
		return;
	}
	for (auto &row_child : function.GetChildrenMutable()) {
		result.push_back(std::move(row_child));
	}
}

BoundStatement ExpressionBinder::BindSubqueryNode(QueryNode &node, shared_ptr<Binder> &subquery_binder,
                                                  bool try_subquery) {
	// bind the subquery in its own binder
	subquery_binder = Binder::CreateBinder(context, binder);
	subquery_binder->SetCanContainNulls(true);
	subquery_binder->SetInsideSubquery();
	if (try_subquery) {
		// TRY is lowered onto the bound plan of this subquery after binding
		subquery_binder->SetBindingTrySubquery();
	}

	subquery_binder->BeginSubqueryBind(binder, *this);
	auto bound_node = subquery_binder->BindNode(node);
	subquery_binder->FinishSubqueryBind();

	// check the correlated columns of the subquery for correlated columns with depth > 1
	for (idx_t i = 0; i < subquery_binder->correlated_columns.size(); i++) {
		CorrelatedColumnInfo corr = subquery_binder->correlated_columns[i];
		if (corr.depth > 1) {
			// depth > 1, the column references the query ABOVE the current one
			// add to the set of correlated columns for THIS query
			corr.depth -= 1;
			binder.AddCorrelatedColumn(corr);
		}
	}
	return bound_node;
}

BindResult ExpressionBinder::BindExpression(SubqueryExpression &expr, idx_t depth) {
	// consume the one-shot marker BindTrySubquery sets for the subquery it is about to bind
	const bool try_subquery = bind_try_subquery;
	bind_try_subquery = false;
	bool rethrow_try_errors = false;
	bool tolerant_lowering = false;
	if (inside_try) {
		if (expr.GetSubqueryType() != SubqueryType::SCALAR) {
			throw BinderException("TRY can not be used in combination with EXISTS/IN/ANY/ALL subqueries");
		}
		// a scalar subquery nested deeper inside a TRY argument: lower TRY onto it like a direct TRY
		// subquery, and re-raise captured errors at the use site where the enclosing TRY masks them
		CheckTrySubqueryNode(expr, expr);
		rethrow_try_errors = true;
	} else if (!try_subquery && transport_try_errors && expr.GetSubqueryType() == SubqueryType::SCALAR &&
	           expr.Subquery()->node->type == QueryNodeType::SELECT_NODE) {
		// a scalar subquery inside the select item of a TRY subquery: lower it with an exposed error
		// flag, so the enclosing lowering masks its errors like those of the surrounding expression;
		// shapes the lowering cannot cover fall back to the plain subquery, whose errors propagate
		rethrow_try_errors = true;
		tolerant_lowering = true;
	}
	auto original_expression = case_operand_binding ? expr.Copy() : nullptr;
	shared_ptr<Binder> subquery_binder;
	auto bound_node = BindSubqueryNode(*expr.Subquery()->node, subquery_binder, try_subquery || rethrow_try_errors);
	// bind the child expression the subquery is compared against, if any; the child ends up in the
	// join condition of the flattened subquery, where a transported error flag has no consumer
	unique_ptr<Expression> bound_child;
	if (expr.GetChild()) {
		const auto save_transport_try_errors = transport_try_errors;
		transport_try_errors = false;
		auto child_result = Bind(expr.GetChildMutable(), depth);
		transport_try_errors = save_transport_try_errors;
		if (child_result.HasError()) {
			return child_result;
		}
		bound_child = std::move(child_result.expression);
	}
	vector<unique_ptr<Expression>> child_expressions;
	if (expr.GetSubqueryType() != SubqueryType::EXISTS) {
		idx_t expected_columns = 1;
		bool has_unexpanded_struct = false;
		if (expr.GetChild()) {
			auto child = std::move(bound_child);
			// Check if child is an unexpanded struct before extraction
			has_unexpanded_struct = TypeIsTuple(child->GetReturnType());
			ExtractSubqueryChildren(child, child_expressions, bound_node.types, expr.GetComparisonType());
			if (child_expressions.empty()) {
				child_expressions.push_back(std::move(child));
			}
			expected_columns = child_expressions.size();
		}
		// If we keep a row-valued child intact, we still need to distinguish between:
		// (1) a subquery that returns a single row/struct value and
		// (2) a subquery that returns multiple scalar columns that should match the row width.
		if (has_unexpanded_struct && expected_columns == 1 && TypeIsTuple(child_expressions[0]->GetReturnType())) {
			const auto struct_child_count = StructType::GetChildCount(child_expressions[0]->GetReturnType());
			const bool subquery_returns_single_struct =
			    bound_node.types.size() == 1 && TypeIsTuple(bound_node.types[0]);
			if (!subquery_returns_single_struct) {
				// The child is a row with N elements, so a scalar/multi-column subquery must expose N columns.
				// This preserves the historical width mismatch error for cases like:
				// (a, b) = ALL(SELECT 1)
				expected_columns = struct_child_count;
			}
		}
		if (bound_node.types.size() != expected_columns) {
			throw BinderException(expr, "Subquery returns %zu columns - expected %d", bound_node.types.size(),
			                      expected_columns);
		}
	}
	LogicalType return_type =
	    expr.GetSubqueryType() == SubqueryType::SCALAR ? bound_node.types[0] : LogicalType(LogicalTypeId::BOOLEAN);
	if (rethrow_try_errors && bound_node.plan) {
		// lower TRY onto the plan and expose the error flag in a struct next to the masked value;
		// the expression keeps the value type, the struct is unpacked again when the subquery is planned
		auto &indexes = subquery_binder->GetTrySubqueryPlanIndexes();
		if (!indexes.recorded) {
			throw InternalException("TRY subquery: no plan indexes were recorded for the top select node");
		}
		auto struct_type = LowerTrySubquery(expr, indexes, *bound_node.plan, true, tolerant_lowering);
		if (struct_type) {
			bound_node.types[0] = *struct_type;
		} else {
			// fall back to the plain subquery, whose errors propagate
			rethrow_try_errors = false;
		}
	}
	if (return_type.id() == LogicalTypeId::UNKNOWN) {
		return_type = LogicalType::SQLNULL;
	}

	auto result = make_uniq<BoundSubqueryExpression>(return_type);
	if (expr.GetSubqueryType() == SubqueryType::ANY) {
		// ANY comparison
		// cast child and subquery child to equivalent types
		// Special case: if we have a single struct child and multiple subquery types,
		// this means we kept the struct intact for ordered comparison (e.g., (a,b) < ANY(...))
		if (child_expressions.size() == 1 && bound_node.types.size() > 1 &&
		    TypeIsTuple(child_expressions[0]->GetReturnType())) {
			auto &child = child_expressions[0];
			auto child_type = ExpressionBinder::GetExpressionReturnType(*child);
			auto subquery_type = LogicalType::TUPLE(bound_node.types);
			LogicalType compare_type;
			if (!LogicalType::TryGetMaxLogicalType(context, child_type, subquery_type, compare_type)) {
				throw BinderException(
				    expr, "Cannot compare values of type %s and %s in IN/ANY/ALL clause - an explicit cast is required",
				    child_type.ToString(), subquery_type);
			}
			child = BoundCastExpression::AddCastToType(context, std::move(child), compare_type);
			result->GetChildrenMutable().push_back(std::move(child));
			for (idx_t child_idx = 0; child_idx < bound_node.types.size(); child_idx++) {
				auto &subquery_child_type = bound_node.types[child_idx];
				result->ChildTypesMutable().push_back(subquery_child_type);
				result->ChildTargetsMutable().push_back(StructType::GetChildType(compare_type, child_idx));
			}
		} else {
			// Standard case: either no struct or struct was extracted into separate expressions
			for (idx_t child_idx = 0; child_idx < child_expressions.size(); child_idx++) {
				auto &child = child_expressions[child_idx];
				auto child_type = ExpressionBinder::GetExpressionReturnType(*child);
				auto &subquery_type = bound_node.types[child_idx];
				LogicalType compare_type;
				if (!LogicalType::TryGetMaxLogicalType(context, child_type, subquery_type, compare_type)) {
					throw BinderException(
					    expr,
					    "Cannot compare values of type %s and %s in IN/ANY/ALL clause - an explicit cast is required",
					    child_type.ToString(), subquery_type);
				}
				child = BoundCastExpression::AddCastToType(context, std::move(child), compare_type);
				result->ChildTypesMutable().push_back(subquery_type);
				result->ChildTargetsMutable().push_back(compare_type);
				result->GetChildrenMutable().push_back(std::move(child));
			}
		}
	}
	result->GetBinderMutable() = std::move(subquery_binder);
	result->SubqueryMutable() = std::move(bound_node);
	result->SubqueryTypeMutable() = expr.GetSubqueryType();
	result->ComparisonTypeMutable() = expr.GetComparisonType();
	if (rethrow_try_errors) {
		result->SetRethrowTryErrors();
		if (tolerant_lowering) {
			// record on the enclosing TRY select node that its item carries a re-raise only its own
			// lowering can consume, so that lowering must not fall back to the plain subquery
			binder.GetTrySubqueryPlanIndexesMutable().nested_transport = true;
		}
	}

	if (original_expression) {
		case_operand_binding->Remember(*result, original_expression->Cast<SubqueryExpression>(), try_subquery);
	}

	return BindResult(std::move(result));
}

} // namespace duckdb
