#include "duckdb/planner/expression_nullability.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include <algorithm>

namespace duckdb {

bool ExpressionBecomesNull(const Expression &expr,
                           const std::function<bool(const BoundColumnRefExpression &)> &column_becomes_null) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = expr.Cast<BoundColumnRefExpression>();
		return column_becomes_null(column);
	}
	if (!expr.PropagatesNullValues()) {
		return false;
	}
	bool child_becomes_null = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		child_becomes_null |= ExpressionBecomesNull(child, column_becomes_null);
	});
	return child_becomes_null;
}

NotNullExpressionAnalyzer::NotNullExpressionAnalyzer(ClientContext &context_p,
                                                     optional_ptr<LogicalOperator> plan_root_p)
    : context(context_p), plan_root(plan_root_p) {
}

static bool GetColumnRefBinding(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	if (colref.Depth() != 0) {
		return false;
	}
	binding = colref.Binding();
	return true;
}

bool FilterRejectsNull(const Expression &filter, const Expression &expr) {
	if (filter.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conjunction = filter.Cast<BoundConjunctionExpression>();
		for (auto &child : conjunction.GetChildren()) {
			if (FilterRejectsNull(*child, expr)) {
				return true;
			}
		}
		return false;
	}
	if (filter.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		auto &conjunction = filter.Cast<BoundConjunctionExpression>();
		if (conjunction.GetChildren().empty()) {
			return false;
		}
		for (auto &child : conjunction.GetChildren()) {
			if (!FilterRejectsNull(*child, expr)) {
				return false;
			}
		}
		return true;
	}
	if (filter.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
		auto &op = filter.Cast<BoundOperatorExpression>();
		return !op.GetChildren().empty() && Expression::Equals(*op.GetChildren()[0], expr);
	}
	if (!BoundComparisonExpression::IsComparison(filter) ||
	    filter.GetExpressionType() == ExpressionType::COMPARE_DISTINCT_FROM ||
	    filter.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return false;
	}
	auto &comparison = filter.Cast<BoundFunctionExpression>();
	return Expression::Equals(BoundComparisonExpression::Left(comparison), expr) ||
	       Expression::Equals(BoundComparisonExpression::Right(comparison), expr);
}

static bool JoinOutputPreservesChildNonNullability(JoinType join_type, idx_t child_idx) {
	switch (join_type) {
	case JoinType::INNER:
		return child_idx < 2;
	case JoinType::LEFT:
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
	case JoinType::SINGLE:
		return child_idx == 0;
	case JoinType::RIGHT:
	case JoinType::RIGHT_SEMI:
	case JoinType::RIGHT_ANTI:
		return child_idx == 1;
	case JoinType::OUTER:
	case JoinType::INVALID:
		return false;
	}
	return false;
}

optional_ptr<LogicalCTE> NotNullExpressionAnalyzer::FindCTE(TableIndex cte_index) {
	if (!plan_root) {
		return nullptr;
	}
	vector<reference<LogicalOperator>> operators;
	operators.push_back(*plan_root);
	while (!operators.empty()) {
		auto &op = operators.back().get();
		operators.pop_back();
		if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE &&
		    op.Cast<LogicalCTE>().table_index == cte_index) {
			return op.Cast<LogicalCTE>();
		}
		for (auto &child : op.children) {
			operators.push_back(*child);
		}
	}
	return nullptr;
}

bool NotNullExpressionAnalyzer::IsNotNull(LogicalOperator &op, const Expression &expr, vector<TableIndex> &seen_ctes) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return false;
		}
		ColumnBinding binding;
		if (!GetColumnRefBinding(expr, binding)) {
			return false;
		}
		auto projection_bindings = projection.GetColumnBindings();
		for (idx_t idx = 0; idx < projection_bindings.size(); idx++) {
			if (projection_bindings[idx] == binding) {
				return IsNotNull(*projection.children[0], *projection.expressions[idx], seen_ctes);
			}
		}
		return false;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		for (auto &filter_expr : filter.expressions) {
			if (FilterRejectsNull(*filter_expr, expr)) {
				return true;
			}
		}
		return filter.children.size() == 1 && IsNotNull(*filter.children[0], expr, seen_ctes);
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.children.size() != 2) {
			return false;
		}
		ColumnBinding binding;
		if (!GetColumnRefBinding(expr, binding)) {
			return false;
		}
		auto output_bindings = join.GetColumnBindings();
		if (std::find(output_bindings.begin(), output_bindings.end(), binding) == output_bindings.end()) {
			return false;
		}
		optional_idx binding_child;
		for (idx_t child_idx = 0; child_idx < join.children.size(); child_idx++) {
			auto child_bindings = join.children[child_idx]->GetColumnBindings();
			if (std::find(child_bindings.begin(), child_bindings.end(), binding) == child_bindings.end()) {
				continue;
			}
			if (binding_child.IsValid()) {
				return false;
			}
			binding_child = child_idx;
		}
		if (!binding_child.IsValid() ||
		    !JoinOutputPreservesChildNonNullability(join.join_type, binding_child.GetIndex())) {
			return false;
		}
		return IsNotNull(*join.children[binding_child.GetIndex()], expr, seen_ctes);
	}
	case LogicalOperatorType::LOGICAL_GET: {
		ColumnBinding binding;
		if (!GetColumnRefBinding(expr, binding)) {
			return false;
		}
		auto &get = op.Cast<LogicalGet>();
		if (binding.table_index != get.table_index) {
			return false;
		}
		if (get.table_filters.HasFilter(binding.column_index)) {
			auto column_expr = make_uniq<BoundColumnRefExpression>(expr.GetReturnType(), binding);
			auto filter_expr =
			    get.table_filters.GetFilterByColumnIndex(binding.column_index).ToExpression(*column_expr);
			if (FilterRejectsNull(*filter_expr, expr)) {
				return true;
			}
		}
		auto table = get.GetTable();
		if (!table) {
			return false;
		}
		auto &column_index = get.GetColumnIndex(binding);
		if (!column_index.HasPrimaryIndex() || column_index.HasChildren() ||
		    column_index.GetPrimaryIndex() == DConstants::INVALID_INDEX) {
			return false;
		}
		auto stats = table->GetStatistics(context, column_index.GetPrimaryIndex());
		// unknown statistics must not count as proof of non-nullability
		return stats && stats->CanHaveNoNull() && !stats->CanHaveNull();
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		ColumnBinding binding;
		if (!GetColumnRefBinding(expr, binding)) {
			return false;
		}
		auto &cte_ref = op.Cast<LogicalCTERef>();
		if (binding.table_index != cte_ref.table_index ||
		    std::find(seen_ctes.begin(), seen_ctes.end(), cte_ref.cte_index) != seen_ctes.end()) {
			return false;
		}
		auto cte = FindCTE(cte_ref.cte_index);
		if (!cte || cte->children.empty()) {
			return false;
		}
		auto column_index = binding.column_index.GetIndex();
		auto &cte_source = *cte->children[0];
		auto source_bindings = cte_source.GetColumnBindings();
		if (column_index >= source_bindings.size()) {
			return false;
		}
		seen_ctes.push_back(cte_ref.cte_index);
		auto source_expr = BoundColumnRefExpression(expr.GetReturnType(), source_bindings[column_index]);
		auto result = IsNotNull(cte_source, source_expr, seen_ctes);
		seen_ctes.pop_back();
		return result;
	}
	default:
		return false;
	}
}

bool NotNullExpressionAnalyzer::IsNotNull(LogicalOperator &op, const Expression &expr) {
	vector<TableIndex> seen_ctes;
	return IsNotNull(op, expr, seen_ctes);
}

} // namespace duckdb
