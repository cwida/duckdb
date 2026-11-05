#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/common/enums/table_function_identifier_conversion.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/table_binding.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

TableFunctionBinder::TableFunctionBinder(Binder &binder, ClientContext &context, string table_function_name_p,
                                         string clause_p, IdentifierConversionPolicy identifier_conversion_policy_p)
    : ExpressionBinder(binder, context), table_function_name(std::move(table_function_name_p)),
      clause(std::move(clause_p)), identifier_conversion_policy(identifier_conversion_policy_p) {
}

BindResult TableFunctionBinder::BindLambdaReference(LambdaRefExpression &expr, idx_t depth) {
	D_ASSERT(lambda_bindings && expr.LambdaIndex() < lambda_bindings->size());
	auto &lambda_ref = expr.Cast<LambdaRefExpression>();
	return (*lambda_bindings)[expr.LambdaIndex()].Bind(lambda_ref, depth);
}

BindResult TableFunctionBinder::BindColumnReference(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                    bool root_expression) {
	auto &col_ref = expr_ptr->Cast<ColumnRefExpression>();
	if (!col_ref.IsQualified()) {
		// Try binding as a lambda parameter.
		auto lambda_ref = LambdaRefExpression::FindMatchingBinding(lambda_bindings, col_ref.GetColumnName());
		if (lambda_ref) {
			return BindLambdaReference(lambda_ref->Cast<LambdaRefExpression>(), depth);
		}

		if (binder.macro_binding && binder.macro_binding->HasMatchingBinding(col_ref.GetColumnName())) {
			throw ParameterNotResolvedException();
		}
	} else if (col_ref.ColumnNames()[0].StartsWith(DummyBinding::DUMMY_NAME) && binder.macro_binding &&
	           binder.macro_binding->HasMatchingBinding(col_ref.GetColumnName())) {
		throw ParameterNotResolvedException();
	}

	auto query_location = col_ref.GetQueryLocation();
	auto column_names = col_ref.ColumnNames();
	auto result_name = StringUtil::Join(column_names, ".");
	// the name is never a column of this scope, so it can only come from an enclosing one. Resolving it
	// there is not enough to call it a lateral parameter: a scope can reach a name and still fail to
	// bind it, in which case the name falls through to being read as a string literal instead.
	// BindInEnclosingScope performs the whole outward walk - installing each scope's qualified form and
	// passing over the scopes that cannot bind it - so what is left is a real bind, as it was before.
	ErrorData not_a_parameter(ExceptionType::BINDER,
	                          StringUtil::Format("Referenced column \"%s\" not found in FROM clause!", result_name));
	if (!table_function_name.empty()) {
		if (!BindInEnclosingScope(col_ref, depth, expr_ptr, not_a_parameter).HasError()) {
			// it is a lateral join parameter - this is not supported in this type of table function
			throw BinderException(query_location,
			                      "Table function \"%s\" does not support lateral join column parameters - cannot use "
			                      "column \"%s\" in this context.\nThe function only supports literals as parameters.",
			                      table_function_name, result_name);
		}
	}

	if (identifier_conversion_policy != IdentifierConversionPolicy::ALLOW) {
		auto value_function = ExpressionBinder::GetSQLValueFunction(column_names.back());
		if (value_function) {
			return BindExpression(value_function, depth, root_expression);
		}
	}

	if (table_function_name.empty()) {
		// a COLUMNS expression: an enclosing scope may still own the name
		auto result = BindInEnclosingScope(col_ref, depth, expr_ptr, std::move(not_a_parameter));
		if (!result.HasError()) {
			return result;
		}
		throw BinderException(query_location,
		                      "Failed to bind \"%s\" - COLUMNS expression can only contain lambda parameters",
		                      result_name);
	}

	auto setting = identifier_conversion_policy == IdentifierConversionPolicy::ALLOW
	                   ? TableFunctionIdentifierConversion::ENABLE_IMPLICIT_STRING
	                   : Settings::Get<TableFunctionIdentifierConversionSetting>(context);
	auto implicit_conversion_disabled = setting == TableFunctionIdentifierConversion::DISABLE_IMPLICIT_STRING;
	auto warn_implicit_conversion = setting == TableFunctionIdentifierConversion::DEFAULT;
	const auto msg =
	    StringUtil::Format("Deprecated implicit conversion of unbound identifiers to strings in table function "
	                       "arguments detected. Please use a string literal instead, e.g. %s.\n"
	                       "Use SET table_function_identifier_conversion='ENABLE_IMPLICIT_STRING' to revert to the "
	                       "deprecated behavior.",
	                       SQLString::ToString(result_name));
	if (implicit_conversion_disabled) {
		throw BinderException(query_location, msg);
	}
	if (warn_implicit_conversion) {
		DUCKDB_LOG_WARNING(context, msg);
	}
	return BindResult(make_uniq<BoundConstantExpression>(Value(result_name)));
}

BindResult TableFunctionBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                               bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::LAMBDA_REF:
		return BindLambdaReference(expr.Cast<LambdaRefExpression>(), depth);
	case ExpressionClass::COLUMN_REF:
		return BindColumnReference(expr_ptr, depth, root_expression);
	case ExpressionClass::SUBQUERY:
		throw BinderException(clause + " cannot contain subqueries");
	case ExpressionClass::DEFAULT:
		return BindResult(clause + " cannot contain DEFAULT clause");
	case ExpressionClass::WINDOW:
		return BindResult(clause + " cannot contain window functions!");
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

string TableFunctionBinder::UnsupportedAggregateMessage() {
	return clause + " cannot contain aggregates!";
}

} // namespace duckdb
