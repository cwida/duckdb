#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/bind_context.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_lambdaref_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! The number of parameters of the enclosing lambdas that are passed to a nested lambda function, i.e. without the
//! parameters of the lambdas enclosing an aggregate
static idx_t GetLambdaParamCount(vector<DummyBinding> &lambda_bindings, idx_t inaccessible_lambda_binding_count) {
	idx_t count = 0;
	for (idx_t i = inaccessible_lambda_binding_count; i < lambda_bindings.size(); i++) {
		count += lambda_bindings[i].GetColumnCount();
	}
	return count;
}

static idx_t GetLambdaParamIndex(vector<DummyBinding> &lambda_bindings, const BoundLambdaExpression &bound_lambda_expr,
                                 const BoundLambdaRefExpression &bound_lambda_ref_expr) {
	D_ASSERT(bound_lambda_ref_expr.LambdaIndex() < lambda_bindings.size());
	idx_t offset = 0;
	// count the remaining lambda parameters BEFORE the current lambda parameter,
	// as these will be in front of the current lambda parameter in the input chunk
	for (idx_t i = bound_lambda_ref_expr.LambdaIndex() + 1; i < lambda_bindings.size(); i++) {
		offset += lambda_bindings[i].GetColumnCount();
	}
	offset += lambda_bindings[bound_lambda_ref_expr.LambdaIndex()].GetColumnCount() -
	          bound_lambda_ref_expr.Binding().column_index - 1;
	offset += bound_lambda_expr.ParameterCount();
	return offset;
}

static void ExtractParameter(const ParsedExpression &expr, vector<Identifier> &column_names,
                             vector<string> &column_aliases) {
	auto &column_ref = expr.Cast<ColumnRefExpression>();
	if (column_ref.IsQualified()) {
		throw BinderException(LambdaExpression::InvalidParametersErrorMessage());
	}

	column_names.emplace_back(column_ref.GetName());
	column_aliases.push_back(column_ref.ToString());
}

static void ExtractParameters(LambdaExpression &expr, vector<Identifier> &column_names,
                              vector<string> &column_aliases) {
	// extract the lambda parameters, which are a single column
	// reference, or a list of column references (ROW function)
	string error_message;
	auto column_refs = expr.ExtractColumnRefExpressions(error_message);
	if (!error_message.empty()) {
		throw BinderException(error_message);
	}

	for (const auto &column_ref : column_refs) {
		ExtractParameter(column_ref.get(), column_names, column_aliases);
	}
	D_ASSERT(!column_names.empty());
}

static bool IsDoubleArrowRHS(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<FunctionExpression>();
	return func.IsOperator() && func.FunctionName() == "->>" && func.GetArguments().size() == 2;
}

static unique_ptr<ParsedExpression> RestructureArrowChain(LambdaExpression &expr) {
	auto &rhs_func = expr.RightMutable()->Cast<FunctionExpression>();
	auto inner_lambda = make_uniq<LambdaExpression>(
	    std::move(expr.LeftMutable()), std::move(rhs_func.GetArgumentsMutable()[0].GetExpressionMutable()));
	inner_lambda->GetLambdaSyntaxTypeMutable() = expr.GetLambdaSyntaxType();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(inner_lambda));
	children.push_back(std::move(rhs_func.GetArgumentsMutable()[1].GetExpressionMutable()));
	auto restructured = make_uniq<FunctionExpression>("->>", std::move(children));
	restructured->IsOperatorMutable() = true;
	return std::move(restructured);
}

BindResult ExpressionBinder::BindExpression(LambdaExpression &expr, idx_t depth,
                                            const vector<LogicalType> &function_child_types,
                                            optional_ptr<bind_lambda_function_t> bind_lambda_function,
                                            optional_ptr<BindLambdaContext> bind_lambda_context) {
	if (expr.GetLambdaSyntaxType() == LambdaSyntaxType::LAMBDA_KEYWORD && !bind_lambda_function) {
		return BindResult("invalid lambda expression");
	}

	if (!bind_lambda_function) {
		// The PEG parser produces A -> (B ->> C) where the standard parser left-associates to (A -> B) ->> C.
		// Restructure to match standard behavior before binding.
		if (IsDoubleArrowRHS(expr.Right())) {
			unique_ptr<ParsedExpression> restructured = RestructureArrowChain(expr);
			return BindExpression(restructured, depth);
		}

		// This is not a lambda expression, but the JSON arrow operator.
		// Remember the original expression in case of a binding error.
		if (!expr.CopiedExprMutable()) {
			expr.CopiedExprMutable() = expr.Right().Copy();
		}
		OperatorExpression arrow_expr(ExpressionType::ARROW, std::move(expr.LeftMutable()),
		                              std::move(expr.RightMutable()));
		auto bind_result = BindExpression(arrow_expr, depth);

		// The arrow_expr now might contain bound nodes.
		// Restore the original expression.
		if (bind_result.HasError()) {
			D_ASSERT(arrow_expr.GetChildrenMutable().size() == 2);
			expr.LeftMutable() = std::move(arrow_expr.GetChildrenMutable()[0]);
			expr.RightMutable() = std::move(arrow_expr.GetChildrenMutable()[1]);
		}
		return bind_result;
	}

	// extract and verify lambda parameters to create dummy columns
	vector<LogicalType> column_types;
	vector<Identifier> column_names;
	vector<string> column_aliases;
	ExtractParameters(expr, column_names, column_aliases);
	for (idx_t i = 0; i < column_names.size(); i++) {
		column_types.push_back((*bind_lambda_function)(context, function_child_types, i, bind_lambda_context));
	}

	// base table alias
	auto table_alias = StringUtil::Join(column_aliases, ", ");
	if (column_aliases.size() > 1) {
		table_alias = "(" + table_alias + ")";
	}

	// create a lambda binding and push it to the lambda bindings vector
	vector<DummyBinding> local_bindings;
	if (!lambda_bindings) {
		lambda_bindings = &local_bindings;
	}
	DummyBinding new_lambda_binding(column_types, column_names, table_alias);
	lambda_bindings->push_back(new_lambda_binding);

	if (expr.CopiedExprMutable()) {
		expr.RightMutable() = std::move(expr.CopiedExprMutable());
	}
	auto result = BindExpression(expr.RightMutable(), depth, false);
	lambda_bindings->pop_back();

	// successfully bound a subtree of nested lambdas, set this to nullptr in case other parts of the
	// query also contain lambdas
	if (lambda_bindings->empty()) {
		lambda_bindings = nullptr;
	}

	if (result.HasError()) {
		result.error.Throw();
	}

	auto bound_lambda = make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA,
	                                                     std::move(result.expression), column_names.size());
	bound_lambda->SetParameterNames(std::move(column_names));
	return BindResult(std::move(bound_lambda));
}

void ExpressionBinder::TransformCapturedLambdaColumn(unique_ptr<Expression> &original,
                                                     unique_ptr<Expression> &replacement,
                                                     BoundLambdaExpression &bound_lambda_expr,
                                                     const optional_ptr<bind_lambda_function_t> bind_lambda_function,
                                                     const optional_ptr<BindLambdaContext> bind_lambda_context,
                                                     const vector<LogicalType> &function_child_types) {
	// check if the original expression is a lambda parameter
	if (original->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA_REF) {
		auto &bound_lambda_ref = original->Cast<BoundLambdaRefExpression>();
		auto alias = bound_lambda_ref.GetAlias();

		// refers to a lambda parameter outside the current lambda function
		// so the lambda parameter will be inside the lambda_bindings
		if (lambda_bindings && bound_lambda_ref.LambdaIndex() != lambda_bindings->size()) {
			auto &binding = (*lambda_bindings)[bound_lambda_ref.LambdaIndex()];
			auto &column_names = binding.GetColumnNames();
			auto &column_types = binding.GetColumnTypes();
			D_ASSERT(column_names.size() == column_types.size());

			// find the matching dummy column in the lambda binding
			for (idx_t column_idx = 0; column_idx < binding.GetColumnCount(); column_idx++) {
				if (ProjectionIndex(column_idx) == bound_lambda_ref.Binding().column_index) {
					// now create the replacement
					auto index = GetLambdaParamIndex(*lambda_bindings, bound_lambda_expr, bound_lambda_ref);
					replacement =
					    make_uniq<BoundReferenceExpression>(column_names[column_idx], column_types[column_idx], index);
					return;
				}
			}

			// error resolving the lambda index
			throw InternalException("Failed to bind lambda parameter internally");
		}
		// refers to a lambda parameter inside the current lambda function
		auto logical_type = (*bind_lambda_function)(context, function_child_types,
		                                            bound_lambda_ref.Binding().column_index, bind_lambda_context);
		auto index = bound_lambda_expr.ParameterCount() - bound_lambda_ref.Binding().column_index - 1;
		replacement = make_uniq<BoundReferenceExpression>(alias, logical_type, index);
		return;
	}

	// this is not a lambda parameter, get the capture offset
	idx_t offset = 0;
	if (lambda_bindings) {
		offset += GetLambdaParamCount(*lambda_bindings, inaccessible_lambda_binding_count);
	}
	offset += bound_lambda_expr.ParameterCount();
	offset += bound_lambda_expr.CapturesMutable().size();

	replacement = make_uniq<BoundReferenceExpression>(original->GetAlias(), original->GetReturnType(), offset);
	bound_lambda_expr.CapturesMutable().push_back(std::move(original));
}

void ExpressionBinder::CaptureLambdaColumns(BoundLambdaExpression &bound_lambda_expr, unique_ptr<Expression> &expr,
                                            const optional_ptr<bind_lambda_function_t> bind_lambda_function,
                                            const optional_ptr<BindLambdaContext> bind_lambda_context,
                                            const vector<LogicalType> &function_child_types) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		throw BinderException("subqueries in lambda expressions are not supported");
	}

	// lambdas are bound depth-first, so an inner lambda has already had its own columns captured by the
	// time we get here - its captures are children of the function it belongs to, which we do visit
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
		return;
	}

	// we do not need to replace anything, as these will be constant in the lambda expression
	// when executed by the expression executor
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return;
	}

	// these expression classes do not have children, transform them
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
	    expr->GetExpressionClass() == ExpressionClass::BOUND_PARAMETER ||
	    expr->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA_REF) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			// Search for UNNEST.
			auto &column_binding = expr->Cast<BoundColumnRefExpression>().Binding();
			ThrowIfUnnestInLambda(column_binding);
		}

		// move the expr because we are going to replace it
		auto original = std::move(expr);
		unique_ptr<Expression> replacement;

		TransformCapturedLambdaColumn(original, replacement, bound_lambda_expr, bind_lambda_function,
		                              bind_lambda_context, function_child_types);

		// replace the expression
		expr = std::move(replacement);

	} else {
		// recursively enumerate the children of the expression
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
			CaptureLambdaColumns(bound_lambda_expr, child, bind_lambda_function, bind_lambda_context,
			                     function_child_types);
		});
	}

	expr->Verify();
}

} // namespace duckdb
