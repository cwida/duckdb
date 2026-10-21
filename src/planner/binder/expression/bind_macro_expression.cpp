#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/catalog/catalog_entry/language_catalog_entry.hpp"
#include "duckdb/function/language_macro_function.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/language_function_binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"

namespace duckdb {

void ExpressionBinder::ReplaceMacroParametersInLambda(FunctionExpression &function,
                                                      vector<identifier_set_t> &lambda_params) {
	for (auto &child : function.GetArgumentsMutable()) {
		if (child.GetExpression().GetExpressionClass() != ExpressionClass::LAMBDA) {
			ReplaceMacroParameters(child.GetExpressionMutable(), lambda_params);
			continue;
		}

		// Special-handling for LHS lambda parameters.
		// We do not replace them, and we add them to the lambda_params vector.
		auto &lambda_expr = child.GetExpressionMutable()->Cast<LambdaExpression>();
		string error_message;
		auto column_ref_expressions = lambda_expr.ExtractColumnRefExpressions(error_message);

		if (!error_message.empty()) {
			// Possibly a JSON function, replace both LHS and RHS.
			ReplaceMacroParameters(lambda_expr.LeftMutable(), lambda_params);
			ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);
			continue;
		}

		// Push the lambda parameter names of this level.
		lambda_params.emplace_back();
		for (const auto &column_ref_expr : column_ref_expressions) {
			const auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
			lambda_params.back().emplace(column_ref.GetName());
		}

		// Only replace in the RHS of the expression.
		ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);

		lambda_params.pop_back();
	}
}

void ExpressionBinder::ReplaceMacroParameters(unique_ptr<ParsedExpression> &expr,
                                              vector<identifier_set_t> &lambda_params) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// If the expression is a column reference, we replace it with its argument.
		auto &col_ref = expr->Cast<ColumnRefExpression>();
		if (LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetColumnName())) {
			return;
		}

		bool bind_macro_parameter = false;
		if (col_ref.IsQualified()) {
			// the table qualifier is the component directly before the column name
			auto &names = col_ref.ColumnNames();
			if (names[names.size() - 2].StartsWith(DummyBinding::DUMMY_NAME)) {
				bind_macro_parameter = true;
			}
		} else {
			bind_macro_parameter = macro_binding->HasMatchingBinding(col_ref.GetColumnName());
		}

		if (bind_macro_parameter) {
			D_ASSERT(macro_binding->HasMatchingBinding(col_ref.GetColumnName()));
			expr = macro_binding->ParamToArg(col_ref);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		// Special-handling for lambdas, which are inside function expressions.
		auto &function = expr->Cast<FunctionExpression>();
		if (function.IsLambdaFunction()) {
			return ReplaceMacroParametersInLambda(function, lambda_params);
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &sq = (expr->Cast<SubqueryExpression>()).Subquery();
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *sq->node, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
		break;
	}
	default:
		break;
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
}

// Find aggregate expression children
void ExpressionBinder::FindAggregateExprs(unique_ptr<ParsedExpression> &expr,
                                          vector<reference<unique_ptr<ParsedExpression>>> &exprs) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &fn_expr = expr->Cast<FunctionExpression>();

		// Look up the function in the catalog, check to see if it is actually an aggregate function
		EntryLookupInfo fn_entry(CatalogType::AGGREGATE_FUNCTION_ENTRY, fn_expr.GetQualifiedName());
		auto entry = GetCatalogEntry(fn_entry, OnEntryNotFound::RETURN_NULL);

		if (entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			exprs.push_back(expr);
			return;
		}
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child_expr) { FindAggregateExprs(child_expr, exprs); });
}

void ExpressionBinder::UnfoldWindowMacroExpression(unique_ptr<ParsedExpression> &expr, ScalarMacroFunction &macro_def) {
	auto macro_copy = macro_def.expression->Copy();
	vector<reference<unique_ptr<ParsedExpression>>> aggregate_exprs;
	FindAggregateExprs(macro_copy, aggregate_exprs);

	// Only allowed if the macro body has a single aggregate expression
	if (aggregate_exprs.size() != 1) {
		throw BinderException("Window function macro bodies must contain exactly one aggregate function");
	}

	// The window spec is pushed down to the aggregate function target within the macro body
	unique_ptr<ParsedExpression> &agg_expr_ref = aggregate_exprs[0];
	auto &agg_fn_expr = agg_expr_ref->Cast<FunctionExpression>();

	// Transfer the macro function attributes
	auto &window_expr = expr->Cast<WindowExpression>();
	window_expr.SetQualifiedName(agg_fn_expr.GetQualifiedName());
	window_expr.GetArgumentsMutable().clear();
	for (auto &arg : agg_fn_expr.GetArgumentsMutable()) {
		window_expr.GetArgumentsMutable().push_back(std::move(arg));
	}
	if (!window_expr.Distinct()) {
		window_expr.DistinctMutable() = agg_fn_expr.Distinct();
	}
	if (window_expr.Filter() && agg_fn_expr.Filter()) {
		// Two FILTER clauses: combine
		window_expr.FilterMutable() =
		    make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(window_expr.FilterMutable()),
		                                     std::move(agg_fn_expr.FilterMutable()));
	} else if (agg_fn_expr.Filter()) {
		//	One FILTER from the MACRO
		window_expr.FilterMutable() = std::move(agg_fn_expr.FilterMutable());
	}
	// Transfer argument ORDER BYs
	if (agg_fn_expr.OrderBy()) {
		auto clone = agg_fn_expr.OrderBy()->Copy();
		window_expr.ArgOrdersMutable() = std::move(clone->Cast<OrderModifier>().orders);
	}

	// Replace the aggregate expression with the new window expression
	agg_expr_ref = std::move(expr);
	expr = std::move(macro_copy);
}

MacroFunction &ExpressionBinder::UnfoldMacroExpression(FunctionExpression &function,
                                                       ScalarMacroCatalogEntry &macro_func,
                                                       unique_ptr<ParsedExpression> &expr, idx_t depth) {
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;
	binder.lambda_bindings = lambda_bindings;
	auto bind_result = MacroFunction::BindMacroFunction(binder, macro_func.macros, macro_func.name, function,
	                                                    positional_arguments, named_arguments, depth);
	if (!bind_result.error.empty()) {
		throw BinderException(*expr, bind_result.error);
	}
	auto &macro_def = macro_func.macros[bind_result.function_idx.GetIndex()]->Cast<ScalarMacroFunction>();

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	macro_binding = new_macro_binding.get();

	// replace current expression with stored macro expression
	// special case: If this is a window function, then we need to return a window expression
	if (expr->GetExpressionClass() == ExpressionClass::WINDOW) {
		UnfoldWindowMacroExpression(expr, macro_def);
	} else {
		expr = macro_def.expression->Copy();
	}

	// qualify only the macro parameters with a new empty binder that only knows the macro binding
	auto dummy_binder = Binder::CreateBinder(context);
	dummy_binder->macro_binding = new_macro_binding.get();
	ExpressionBinder::QualifyColumnNames(*dummy_binder, expr);

	// now replace the parameters
	vector<identifier_set_t> lambda_params;
	ReplaceMacroParameters(expr, lambda_params);
	return macro_def;
}

BindResult ExpressionBinder::BindMacro(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func, idx_t depth,
                                       unique_ptr<ParsedExpression> &expr) {
	auto stack_checker = StackCheck(*expr, 3);
	// expanding nested macros can take a long time, allow the query to be interrupted
	context.InterruptCheck();

	// a function entry holds either language members or plain macro members, never both: plain
	// entries keep the original selection, which consumes the call's arguments and replaces the
	// expression wholesale, so regular macros bind without copying the call
	const bool is_language_entry =
	    !macro_func.macros.empty() && macro_func.macros[0]->type == MacroType::LANGUAGE_MACRO;

	optional_ptr<MacroFunction> member;
	BindResult result;
	if (is_language_entry) {
		// validate the arguments, separate positional and named arguments, and select the member to call.
		// selection moves the argument expressions out of the call, so it runs on a copy: this path
		// does not replace the expression, and the original must stay rebindable for callers that
		// retry binding this tree after a soft error
		auto function_copy_owner = function.Copy();
		auto &function_copy = function_copy_owner->Cast<FunctionExpression>();
		vector<unique_ptr<ParsedExpression>> positional_arguments;
		InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;
		binder.lambda_bindings = lambda_bindings;
		auto macro_result = MacroFunction::BindMacroFunction(binder, macro_func.macros, macro_func.name, function_copy,
		                                                     positional_arguments, named_arguments, depth);
		if (!macro_result.error.empty()) {
			throw BinderException(*expr, macro_result.error);
		}
		member = macro_func.macros[macro_result.function_idx.GetIndex()];
		auto &language_member = member->Cast<LanguageMacroFunction>();
		if (expr->GetExpressionClass() == ExpressionClass::WINDOW) {
			throw BinderException(*expr, "Function %s is a %s function and cannot be used as a window function",
			                      macro_func.name, language_member.language);
		}
		// flatten the arguments into parameter declaration order, defaults were already filled in
		auto arguments = std::move(positional_arguments);
		for (idx_t i = arguments.size(); i < language_member.parameters.size(); i++) {
			auto &param_name = language_member.parameters[i]->Cast<ColumnRefExpression>().GetColumnName();
			auto named_it = named_arguments.find(param_name);
			D_ASSERT(named_it != named_arguments.end());
			arguments.push_back(std::move(named_it->second));
		}
		auto language_binder =
		    LanguageCatalogEntry::GetBinder(context, language_member.language, macro_func.name);
		result = language_binder->BindScalarCall(*this, macro_func, language_member, std::move(arguments), depth);
	} else {
		// unfold the macro expression and bind the result
		member = UnfoldMacroExpression(function, macro_func, expr, depth);
		result = BindExpression(expr, depth);
	}
	if (result.HasError()) {
		return result;
	}
	// enforce a declared RETURNS type by casting the member's result to it
	if (member->return_type.id() != LogicalTypeId::UNKNOWN) {
		result.expression =
		    BoundCastExpression::AddCastToType(context, std::move(result.expression), member->return_type);
	}
	return result;
}

} // namespace duckdb
