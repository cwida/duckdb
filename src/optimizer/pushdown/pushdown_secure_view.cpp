#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"

namespace duckdb {

static bool RetainSecureViewFilter(unique_ptr<Expression> &expression, const LogicalSecureView &view) {
	if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = expression->Cast<BoundColumnRefExpression>();
		if (column.Depth() != 0 || view.output_bindings.size() != view.output_expressions.size()) {
			return false;
		}
		for (idx_t i = 0; i < view.output_bindings.size(); i++) {
			if (column.Binding() == view.output_bindings[i]) {
				expression = view.output_expressions[i]->Copy();
				return true;
			}
		}
		return false;
	}
	bool valid = true;
	ExpressionIterator::EnumerateChildren(
	    *expression, [&](unique_ptr<Expression> &child) { valid = RetainSecureViewFilter(child, view) && valid; });
	return valid;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownSecureView(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_SECURE_VIEW);
	// filters can be pushed into a secure view, but only if evaluating them on rows that the view does not emit
	// cannot be observed from the outside. Expressions that can throw an error or have side effects are wrapped in
	// a barrier: those are never pushed past an operator inside the view that removes rows, and are always
	// evaluated after the filters they end up next to.
	auto &secure_view = op->Cast<LogicalSecureView>();
	// freeze the cardinality estimate of the view before anything is pushed into it - otherwise the estimate of the
	// boundary node reports what the statistics of the view contents say about the caller's predicate. Views whose
	// statistics are exposed anyway do not need this - the caller can derive the same estimate above the boundary
	if (!filters.empty() && !secure_view.propagate_statistics && !op->has_estimated_cardinality) {
		op->SetEstimatedCardinality(op->children[0]->EstimateCardinality(optimizer.GetContext()));
	}

	FilterPushdown child_pushdown(optimizer, convert_mark_joins, projection_mode);
	for (auto &f : filters) {
		auto expr = std::move(f->filter);
		if (secure_view.has_source) {
			auto source_filter = expr->Copy();
			if (!RetainSecureViewFilter(source_filter, secure_view)) {
				source_filter.reset();
			}
			secure_view.source_filters.push_back(std::move(source_filter));
		}
		// the operators inside the view are never shown - report the filter as part of the boundary node instead
		secure_view.pushed_filters.push_back(expr->ToString());
		if (ExpressionBarrier::Required(*expr) && !ExpressionBarrier::Contains(*expr)) {
			expr = ExpressionBarrier::Wrap(std::move(expr));
		}
		if (child_pushdown.AddFilter(std::move(expr)) == FilterResult::UNSATISFIABLE) {
			// filter statically evaluates to false, strip tree
			return make_uniq<LogicalEmptyResult>(std::move(op));
		}
	}
	filters.clear();
	child_pushdown.GenerateFilters();

	// note that the boundary node is always kept in place, even if the child collapses into an empty result - the
	// plan outside the view must not depend on anything the optimizer learns about the contents of the view
	op->children[0] = child_pushdown.Rewrite(std::move(op->children[0]));
	return op;
}

} // namespace duckdb
