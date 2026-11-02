#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

LogicalType ExpressionBinder::ResolveNotType(OperatorExpression &op, vector<unique_ptr<Expression>> &children) {
	// NOT expression, cast child to BOOLEAN
	D_ASSERT(children.size() == 1);
	children[0] = BoundCastExpression::AddCastToType(context, std::move(children[0]), LogicalType::BOOLEAN);
	return LogicalType(LogicalTypeId::BOOLEAN);
}

LogicalType ExpressionBinder::ResolveCoalesceType(OperatorExpression &op, vector<unique_ptr<Expression>> &children) {
	if (children.empty()) {
		throw InternalException("IN requires at least a single child node");
	}
	// get the maximum type from the children
	LogicalType max_type = ExpressionBinder::GetExpressionReturnType(*children[0]);
	bool is_in_operator = (op.GetExpressionType() == ExpressionType::COMPARE_IN ||
	                       op.GetExpressionType() == ExpressionType::COMPARE_NOT_IN);
	for (idx_t i = 1; i < children.size(); i++) {
		auto child_return = ExpressionBinder::GetExpressionReturnType(*children[i]);
		if (is_in_operator) {
			// If it's IN/NOT_IN operator, adjust DECIMAL and VARCHAR returned type.
			if (!BoundComparisonExpression::TryBindComparison(context, max_type, child_return, max_type,
			                                                  op.GetExpressionType())) {
				throw BinderException(op,
				                      "Cannot mix values of type %s and %s in %s clause - an explicit cast is required",
				                      max_type.ToString(), child_return.ToString(),
				                      op.GetExpressionType() == ExpressionType::COMPARE_IN ? "IN" : "NOT IN");
			}
		} else {
			// If it's COALESCE operator, don't do extra adjustment.
			if (!LogicalType::TryGetMaxLogicalType(context, max_type, child_return, max_type)) {
				throw BinderException(
				    op, "Cannot mix values of type %s and %s in COALESCE operator - an explicit cast is required",
				    max_type.ToString(), child_return.ToString());
			}
		}
	}

	// cast all children to the same type
	for (auto &child : children) {
		child = BoundCastExpression::AddCastToType(context, std::move(child), max_type);
		if (is_in_operator) {
			// If it's IN/NOT_IN operator, push collation functions.
			ExpressionBinder::PushCollation(context, child, max_type);
		}
	}
	return max_type;
}

LogicalType ExpressionBinder::ResolveOperatorType(OperatorExpression &op, vector<unique_ptr<Expression>> &children) {
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		// IS (NOT) NULL always returns a boolean, and does not cast its children
		if (!children[0]->GetReturnType().IsValid()) {
			throw ParameterNotResolvedException();
		}
		return LogicalType::BOOLEAN;
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
		ResolveCoalesceType(op, children);
		// (NOT) IN always returns a boolean
		return LogicalType::BOOLEAN;
	case ExpressionType::OPERATOR_COALESCE: {
		return ResolveCoalesceType(op, children);
	}
	case ExpressionType::OPERATOR_TRY: {
		return children[0]->GetReturnType();
	}
	case ExpressionType::OPERATOR_HAS_ERROR:
		return LogicalType::BOOLEAN;
	case ExpressionType::OPERATOR_NOT:
		return ResolveNotType(op, children);
	default:
		throw InternalException("Unrecognized expression type for ResolveOperatorType");
	}
}

BindResult ExpressionBinder::BindGroupingFunction(OperatorExpression &op, idx_t depth) {
	return BindResult("GROUPING function is not supported here");
}

//! Bind an operator that is really a call to a scalar function, over children this binder has
//! already bound. The name is resolved exactly as a written function call would be, so that the
//! search path and the binder's catalog-lookup callback still apply.
//! This repeats the tail of BindFunction (catalog lookup, BindScalarFunction, the rebind flag) because
//! that one starts from parsed arguments and would bind these children a second time - keep the two in
//! step. Unlike BindFunction it accepts only a scalar function: a macro shadowing the operator's
//! backing name (say, a user macro called struct_extract) cannot be handed already-bound children, so
//! it is reported rather than invoked.
BindResult ExpressionBinder::BindOperatorAsFunction(OperatorExpression &op, const Identifier &function_name,
                                                    vector<unique_ptr<Expression>> children) {
	if (binder.GetBindingMode() == BindingMode::EXTRACT_NAMES ||
	    binder.GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
		return BindResult(make_uniq<BoundConstantExpression>(Value(LogicalType::SQLNULL)));
	}
	FunctionExpression lookup(function_name, vector<unique_ptr<ParsedExpression>>());
	lookup.SetQueryLocation(op.GetQueryLocation());
	auto &func = BindFunction(lookup);
	if (func.type != CatalogType::SCALAR_FUNCTION_ENTRY) {
		throw BinderException(op, "%s is not a scalar function", function_name.GetIdentifierName());
	}

	ErrorData error;
	FunctionBinder function_binder(binder);
	auto result = function_binder.BindScalarFunction(func.Cast<ScalarFunctionCatalogEntry>(), std::move(children),
	                                                 error, false, &binder);
	if (!result) {
		error.AddQueryLocation(op);
		error.Throw();
	}
	if (result->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &bound_function = result->Cast<BoundFunctionExpression>();
		if (bound_function.Function().GetStability() == FunctionStability::CONSISTENT_WITHIN_QUERY) {
			binder.SetAlwaysRequireRebind();
		}
	}
	return BindResult(std::move(result));
}

//! GROUPING cannot go through DispatchToScope: it is not bound by the scope's BindExpression but by its
//! BindGroupingFunction, which is what registers the grouping set on that scope's select node. The
//! correlated-column registration DispatchToScope would have done is therefore repeated here.
BindResult ExpressionBinder::BindGroupingInEnclosingScope(OperatorExpression &op,
                                                          vector<reference<ParsedExpression>> &children, idx_t owner,
                                                          idx_t depth) {
	ErrorData bind_error;
	for (optional_idx scope = owner; scope.IsValid(); scope = ResolveOuterGroup(children, scope.GetIndex() + 1)) {
		// BindGroupingFunction qualifies the children against the scope binding them, so bind a copy and
		// leave the original intact for the scopes further out
		auto attempt = op.Copy();
		auto result = ScopeAt(scope.GetIndex())
		                  .BindGroupingFunction(attempt->Cast<OperatorExpression>(), depth + scope.GetIndex());
		if (!result.HasError()) {
			ExtractCorrelatedExpressions(binder, *result.expression);
			return result;
		}
		if (!bind_error.HasError()) {
			bind_error = std::move(result.error);
		}
	}
	return BindResult(std::move(bind_error));
}

BindResult ExpressionBinder::BindExpression(OperatorExpression &op, idx_t depth) {
	auto operator_type = op.GetExpressionType();
	if (operator_type == ExpressionType::GROUPING_FUNCTION) {
		// GROUPING reports on the groups of a query, so it belongs to the innermost level that groups
		// by all of its arguments
		if (!binder.GetEnclosingScopes().empty()) {
			vector<reference<ParsedExpression>> children;
			for (auto &child : op.GetChildrenMutable()) {
				children.push_back(*child);
			}
			auto owner = ResolveOuterGroup(children, 0);
			if (owner.IsValid() && owner.GetIndex() != 0) {
				return BindGroupingInEnclosingScope(op, children, owner.GetIndex(), depth);
			}
		}
		return BindGroupingFunction(op, depth);
	}

	// Bind the children of the operator expression. We already create bound expressions.
	// Only those children that trigger an error are not yet bound.
	ErrorData error;
	vector<unique_ptr<Expression>> children;
	if (operator_type == ExpressionType::OPERATOR_TRY) {
		D_ASSERT(op.GetChildrenMutable().size() == 1);
		auto &try_child = op.GetChildrenMutable()[0];
		if (try_child->GetExpressionClass() == ExpressionClass::SUBQUERY &&
		    try_child->Cast<SubqueryExpression>().GetSubqueryType() == SubqueryType::SCALAR) {
			// TRY directly over a scalar subquery binds as an ordinary subquery and lowers onto its plan
			return BindTrySubquery(op, depth);
		}
	}
	if (operator_type == ExpressionType::OPERATOR_TRY || operator_type == ExpressionType::OPERATOR_HAS_ERROR) {
		D_ASSERT(op.GetChildrenMutable().size() == 1);
		const auto was_inside_try = inside_try;
		inside_try = true;
		children.push_back(BindChild(op.GetChildrenMutable()[0], depth, error));
		inside_try = was_inside_try;
	} else {
		for (idx_t i = 0; i < op.GetChildrenMutable().size(); i++) {
			children.push_back(BindChild(op.GetChildrenMutable()[i], depth, error));
		}
	}

	if (error.HasError()) {
		return BindResult(std::move(error));
	}

	// some operators are really scalar functions over the same children
	string function_name;
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_UNPACK:
		return BindResult("UNPACK not allowed here, should have been resolved earlier");
	case ExpressionType::ARRAY_EXTRACT: {
		auto &b_exp = *children[0];
		const auto &b_exp_type = b_exp.GetReturnType();
		if (b_exp_type.id() == LogicalTypeId::MAP) {
			function_name = "map_extract_value";
		} else if (b_exp_type.IsJSONType() && children.size() == 2) {
			function_name = "json_extract";
			// Make sure we only extract array elements, not fields, by adding the $[] syntax
			auto &i_exp = *children[1];
			if (i_exp.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
			    !i_exp.Cast<BoundConstantExpression>().GetValue().IsNull()) {
				auto &const_exp = i_exp.Cast<BoundConstantExpression>();
				auto uinteger_value = const_exp.GetValue().TryCastAs(context, LogicalType::UINTEGER);
				if (uinteger_value) {
					// Array extraction: if the cast fails it's definitely out-of-bounds for a JSON array
					auto index = UIntegerValue::Get(*uinteger_value);
					const_exp.SetValue(StringUtil::Format("$[%lld]", index));
					const_exp.SetReturnType(LogicalType::VARCHAR);
				} else if (const_exp.GetReturnType().id() == LogicalType::VARCHAR) {
					// Field extraction
					const_exp.SetValue(StringUtil::Format("$.\"%s\"", const_exp.GetValue().ToString()));
					const_exp.SetReturnType(LogicalType::VARCHAR);
				}
			}
		} else if (b_exp_type.id() == LogicalTypeId::VARIANT && children.size() == 2) {
			function_name = "variant_extract";
			auto &i_exp = *children[1];
			if (i_exp.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				auto &const_exp = i_exp.Cast<BoundConstantExpression>();
				if (!const_exp.GetValue().IsNull() && const_exp.GetReturnType().IsIntegral()) {
					const_exp.SetValue(const_exp.GetValue().DefaultCastAs(LogicalType::UINTEGER, true));
					const_exp.SetReturnType(LogicalType::UINTEGER);
				}
			}
		} else {
			function_name = "array_extract";
		}
		break;
	}
	case ExpressionType::ARRAY_SLICE:
		function_name = "array_slice";
		break;
	case ExpressionType::STRUCT_EXTRACT: {
		D_ASSERT(children.size() == 2);
		auto &extract_exp = *children[0];
		if (extract_exp.HasParameter() || extract_exp.GetReturnType().id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
		auto &name_exp = *children[1];
		const auto &extract_expr_type = extract_exp.GetReturnType();
		if (extract_expr_type.id() != LogicalTypeId::STRUCT && extract_expr_type.id() != LogicalTypeId::UNION &&
		    extract_expr_type.id() != LogicalTypeId::MAP && extract_expr_type.id() != LogicalTypeId::SQLNULL &&
		    !extract_expr_type.IsJSONType() && extract_expr_type.id() != LogicalTypeId::VARIANT &&
		    extract_expr_type.id() != LogicalTypeId::GEOMETRY) {
			return BindResult(StringUtil::Format("Cannot extract field %s from expression \"%s\" because it is not a "
			                                     "struct, union, map, json or geometry",
			                                     name_exp.ToString(), extract_exp.ToString()));
		}
		if (extract_expr_type.id() == LogicalTypeId::UNION) {
			function_name = "union_extract";
		} else if (extract_expr_type.id() == LogicalTypeId::MAP) {
			function_name = "map_extract_value";
		} else if (extract_expr_type.id() == LogicalTypeId::VARIANT) {
			function_name = "variant_extract";
			auto &i_exp = *children[1];
			if (i_exp.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				auto &const_exp = i_exp.Cast<BoundConstantExpression>();
				if (!const_exp.GetValue().IsNull()) {
					const_exp.SetValue(StringUtil::Format("%s", const_exp.GetValue().ToString()));
					const_exp.SetReturnType(LogicalType::VARCHAR);
				}
			}
		} else if (extract_expr_type.IsJSONType()) {
			function_name = "json_extract";
			// Make sure we only extract fields, not array elements, by adding $. syntax
			if (name_exp.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				auto &const_exp = name_exp.Cast<BoundConstantExpression>();
				if (!const_exp.GetValue().IsNull()) {
					const_exp.SetValue(StringUtil::Format("$.\"%s\"", const_exp.GetValue().ToString()));
					const_exp.SetReturnType(LogicalType::VARCHAR);
				}
			}
		} else if (extract_expr_type.id() == LogicalTypeId::GEOMETRY) {
			function_name = "vertex_extract";
		} else {
			function_name = "struct_extract";
		}
		break;
	}
	case ExpressionType::ARRAY_CONSTRUCTOR:
		function_name = "list_value";
		break;
	case ExpressionType::ARROW:
		function_name = "json_extract";
		break;
	case ExpressionType::OPERATOR_TRY:
	case ExpressionType::OPERATOR_HAS_ERROR: {
		auto &expr = *children[0];
		if (operator_type == ExpressionType::OPERATOR_TRY &&
		    expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY &&
		    !expr.Cast<BoundSubqueryExpression>().RethrowsTryErrors()) {
			// a directly nested TRY over a scalar subquery was already lowered onto the subquery plan
			return BindResult(std::move(children[0]));
		}
		// a rethrowing subquery can surface as the direct child when a binder simplification (such as a
		// same-type cast or single-argument COALESCE) unwrapped to it: TRY must stay to consume the re-raise
		// scalar subqueries deeper inside the argument were lowered during binding and re-raise their
		// captured errors at the use site, so they need no rejection here
		if (expr.IsVolatile()) {
			throw BinderException("%s can not be used in combination with a volatile function",
			                      ExpressionTypeToString(operator_type));
		}
		break;
	}
	default:
		break;
	}
	if (!function_name.empty()) {
		return BindOperatorAsFunction(op, Identifier(function_name), std::move(children));
	}

	// now resolve the types
	LogicalType result_type = ResolveOperatorType(op, children);
	if (op.GetExpressionType() == ExpressionType::OPERATOR_COALESCE) {
		if (children.empty()) {
			throw BinderException("COALESCE needs at least one child");
		}
		if (children.size() == 1) {
			return BindResult(std::move(children[0]));
		}
	}

	auto result = make_uniq<BoundOperatorExpression>(op.GetExpressionType(), result_type);
	for (auto &child : children) {
		result->GetChildrenMutable().push_back(std::move(child));
	}
	return BindResult(std::move(result));
}

} // namespace duckdb
