#include "duckdb/planner/expression_binder/having_binder.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/column_qualifier.hpp"

namespace duckdb {

HavingBinder::HavingBinder(Binder &binder, ClientContext &context, BoundSelectNode &node,
                           AggregateHandling aggregate_handling)
    : BaseSelectBinder(binder, context, node), column_alias_binder(node.bind_state),
      aggregate_handling(aggregate_handling) {
	target_type = LogicalType(LogicalTypeId::BOOLEAN);
}

BindResult HavingBinder::BindLambdaReference(LambdaRefExpression &expr, idx_t depth) {
	D_ASSERT(lambda_bindings && expr.LambdaIndex() < lambda_bindings->size());
	auto &lambda_ref = expr.Cast<LambdaRefExpression>();
	return (*lambda_bindings)[expr.LambdaIndex()].Bind(lambda_ref, depth);
}

BindResult HavingBinder::BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	// Keep the original column name to return a meaningful error message.
	auto col_ref = expr_ptr->Cast<ColumnRefExpression>();
	const auto &column_name = col_ref.GetColumnName();

	if (!col_ref.IsQualified()) {
		// Try binding as a lambda parameter.
		auto lambda_ref = LambdaRefExpression::FindMatchingBinding(lambda_bindings, col_ref.GetColumnName());
		if (lambda_ref) {
			return BindLambdaReference(lambda_ref->Cast<LambdaRefExpression>(), depth);
		}
		// column was not found - check if it is a SQL value function
		auto value_function = GetSQLValueFunction(col_ref.GetColumnName());
		if (value_function) {
			return BindExpression(value_function, depth);
		}
	}

	// Bind the alias.
	BindResult alias_result;
	auto found_alias = column_alias_binder.BindAlias(*this, expr_ptr, depth, root_expression, alias_result);
	if (found_alias) {
		if (depth > 0) {
			throw BinderException("Having clause cannot reference alias %s in correlated subquery", column_name);
		}
		return alias_result;
	}

	if (aggregate_handling != AggregateHandling::FORCE_AGGREGATES) {
		// the name is not a group of this query, but it may belong to an enclosing one
		ErrorData local_error(
		    ExceptionType::BINDER,
		    StringUtil::Format("column %s must appear in the GROUP BY clause or be used in an aggregate function",
		                       column_name));
		return BindInEnclosingScope(expr_ptr->Cast<ColumnRefExpression>(), depth, expr_ptr, std::move(local_error));
	}

	if (depth > 0) {
		throw BinderException("Having clause cannot reference column %s in correlated subquery and group by all",
		                      column_name);
	}

	auto expr = duckdb::BaseSelectBinder::BindColumnRef(expr_ptr, depth, root_expression);
	if (expr.HasError()) {
		return expr;
	}

	// Return a GROUP BY column reference expression.
	auto return_type = expr.expression->GetReturnType();
	auto group_idx = ColumnBinding::PushExpression(node.groups.group_expressions, std::move(expr.expression));
	auto column_binding = ColumnBinding(node.group_index, group_idx);
	auto group_ref = make_uniq<BoundColumnRefExpression>(return_type, column_binding);
	return BindResult(std::move(group_ref));
}

BindResult HavingBinder::BindWindowExpression(WindowExpression &expr, idx_t depth) {
	throw BinderException::Unsupported(expr, "HAVING clause cannot contain window functions!");
}

unique_ptr<ColumnQualifier> HavingBinder::CreateColumnQualifier() {
	return make_uniq<ColumnQualifier>(binder, lambda_bindings, nullptr, *this);
}

void ExpressionBinder::QualifyColumnNames(HavingBinder &having_binder, unique_ptr<ParsedExpression> &expr) {
	ColumnQualifier qualifier(having_binder.binder, having_binder.lambda_bindings, nullptr, having_binder);
	vector<identifier_set_t> lambda_params;
	qualifier.QualifyColumnNames(expr, lambda_params);
}

bool HavingBinder::ClaimsAlias(ColumnRefExpression &colref) {
	return column_alias_binder.DoesColumnAliasExist(colref);
}

} // namespace duckdb
