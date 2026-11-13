#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

static void RevertFilterPullup(LogicalProjection &proj, vector<unique_ptr<Expression>> &expressions) {
	unique_ptr<LogicalFilter> filter = make_uniq<LogicalFilter>();
	for (idx_t i = 0; i < expressions.size(); ++i) {
		filter->expressions.push_back(std::move(expressions[i]));
	}
	expressions.clear();
	filter->children.push_back(std::move(proj.children[0]));
	proj.children[0] = std::move(filter);
}

static void ReplaceExpressionBinding(vector<unique_ptr<Expression>> &proj_expressions, unique_ptr<Expression> &expr,
                                     TableIndex proj_table_idx) {
	expr = ExpressionIterator::PreorderReplace<BoundColumnRefExpression>(
	    std::move(expr), [&](const BoundColumnRefExpression &colref) {
		    // find the corresponding column index in the projection expressions
		    for (idx_t proj_idx = 0; proj_idx < proj_expressions.size(); proj_idx++) {
			    auto &proj_expr = *proj_expressions[proj_idx];
			    if (proj_expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF && colref.Equals(proj_expr)) {
				    return colref.WithBinding(ColumnBinding(proj_table_idx, ProjectionIndex(proj_idx)));
			    }
		    }
		    // Project a new column
		    auto column_index = ColumnBinding::PushExpression(proj_expressions, colref.Copy());
		    return colref.WithBinding(ColumnBinding(proj_table_idx, column_index));
	    });
}

void FilterPullup::ProjectSetOperation(LogicalProjection &proj) {
	vector<unique_ptr<Expression>> copy_proj_expressions;
	// copying the project expressions, it's useful whether we should revert the filter pullup
	for (idx_t i = 0; i < proj.expressions.size(); ++i) {
		copy_proj_expressions.push_back(proj.expressions[i]->Copy());
	}

	// Replace filter expression bindings, when need we add new columns into the copied projection expression
	vector<unique_ptr<Expression>> changed_filter_expressions;
	for (idx_t i = 0; i < filters_expr_pullup.size(); ++i) {
		auto copy_filter_expr = filters_expr_pullup[i]->Copy();
		ReplaceExpressionBinding(copy_proj_expressions, copy_filter_expr, proj.table_index);
		changed_filter_expressions.push_back(std::move(copy_filter_expr));
	}

	/// Case new columns were added into the projection
	// we must skip filter pullup because adding new columns to these operators will change the result
	if (copy_proj_expressions.size() > proj.expressions.size()) {
		RevertFilterPullup(proj, filters_expr_pullup);
		return;
	}

	// now we must replace the filter bindings
	D_ASSERT(filters_expr_pullup.size() == changed_filter_expressions.size());
	for (idx_t i = 0; i < filters_expr_pullup.size(); ++i) {
		filters_expr_pullup[i] = std::move(changed_filter_expressions[i]);
	}
}

unique_ptr<LogicalOperator> FilterPullup::PullupProjection(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_PROJECTION);
	op->children[0] = Rewrite(std::move(op->children[0]));
	if (!filters_expr_pullup.empty()) {
		auto &proj = op->Cast<LogicalProjection>();
		for (auto &expr : filters_expr_pullup) {
			if (expr->CanThrow()) {
				// Keep fallible predicates and their guards below the projection.
				RevertFilterPullup(proj, filters_expr_pullup);
				return op;
			}
		}
		// INTERSECT, EXCEPT, and DISTINCT
		if (!can_add_column) {
			// special treatment for operators that cannot add columns, e.g., INTERSECT, EXCEPT, and DISTINCT
			ProjectSetOperation(proj);
			return op;
		}

		for (idx_t i = 0; i < filters_expr_pullup.size(); ++i) {
			ReplaceExpressionBinding(proj.expressions, filters_expr_pullup[i], proj.table_index);
		}
	}
	return op;
}

} // namespace duckdb
