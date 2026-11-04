#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

/**
 * When a BoundColumnRefExpression that's part of expr (a filter) arrives here, its
 * name will be set to the projection name i.e. "other" for SELECT col as other.
 * If CTE inlining optimizer collapses the CTE in
 * WITH cte AS (SELECT col AS other FROM reader()) SELECT * WHERE other > 0 FROM cte,
 * reader() will get a complex filter with "other" which doesn't exist.
 * Rename the columns back to their original names
 */
static void NormalizeColumnRefAliases(unique_ptr<Expression> &expr, const LogicalGet &get) {
	const vector<ColumnIndex> &column_ids = get.GetColumnIds();
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(expr, [&](auto &ref, auto &) {
		const ColumnBinding &binding = ref.Binding();
		if (binding.table_index != get.table_index || binding.column_index >= column_ids.size()) {
			return;
		}
		const ColumnIndex &col_idx = column_ids[binding.column_index];
		if (!col_idx.HasPrimaryIndex()) {
			ref.SetAlias(Identifier(col_idx.GetFieldName()));
			return;
		}
		const idx_t primary = col_idx.GetPrimaryIndex();
		if (col_idx.IsVirtualColumn()) {
			if (const auto it = get.virtual_columns.find(primary); it != get.virtual_columns.end()) {
				ref.SetAlias(Identifier(it->second.name.GetIdentifierName()));
			}
		} else if (primary < get.names.size()) {
			ref.SetAlias(Identifier(col_idx.GetName(get.names[primary].GetIdentifierName())));
		}
	});
}

void FilterPushdown::PushdownBarrierFilters(LogicalGet &get, vector<unique_ptr<Filter>> &barrier_filters) {
	if (barrier_filters.empty() || !filters.empty()) {
		// nothing to push, or one of the other filters remains on top of the scan - in that case the barrier filters
		// have to stay on top as well, so that they are evaluated after it
		return;
	}
	// only push into a table scan we control the filter order of - external scans may evaluate a pushed-down
	// expression against values (e.g. partition constants) that no surviving row ever has
	auto table = get.GetTable();
	if (!table || !table->IsDuckTable()) {
		return;
	}
	for (idx_t i = 0; i < barrier_filters.size(); i++) {
		auto &expr = *barrier_filters[i]->filter;
		if (expr.IsVolatile()) {
			continue;
		}
		// restrict this to single-column expressions - a multi-column expression is only pushed down partially, and
		// is then also used for zone map pruning
		vector<ColumnBinding> bindings;
		ExtractFilterBindings(expr, bindings);
		if (bindings.empty()) {
			continue;
		}
		bool single_column = true;
		for (idx_t binding_idx = 1; binding_idx < bindings.size(); binding_idx++) {
			if (bindings[binding_idx] != bindings[0]) {
				single_column = false;
				break;
			}
		}
		if (!single_column) {
			continue;
		}
		if (combiner.TryPushdownGenericExpression(get, expr) != FilterPushdownResult::PUSHED_DOWN_FULLY) {
			continue;
		}
		barrier_filters.erase_at(i);
		i--;
	}
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownGet(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_GET);
	auto &get = op->Cast<LogicalGet>();

	for (auto &filter : filters) {
		NormalizeColumnRefAliases(filter->filter, get);
	}

	// hold back the barrier filters: they may only be pushed into the scan if every other filter ends up in the scan
	// as well, since a filter that remains on top of the scan would otherwise run after them
	vector<unique_ptr<Filter>> barrier_filters;
	for (idx_t i = 0; i < filters.size(); i++) {
		if (!filters[i]->has_barrier) {
			continue;
		}
		barrier_filters.push_back(std::move(filters[i]));
		filters.erase_at(i);
		i--;
	}
	auto restore_barrier_filters = [&]() {
		for (auto &barrier_filter : barrier_filters) {
			filters.push_back(std::move(barrier_filter));
		}
		barrier_filters.clear();
	};

	if (get.function.pushdown_complex_filter || get.function.filter_pushdown) {
		// this scan supports some form of filter push-down
		// check if there are any parameters
		// if there are, invalidate them to force a re-bind on execution
		for (auto &filter : filters) {
			if (filter->filter->HasParameter()) {
				// there is a parameter in the filters! invalidate it
				BoundParameterExpression::InvalidateRecursive(*filter->filter);
			}
		}
	}
	const bool assigns_ordinality = get.ordinality_idx.IsValid();
	if (get.function.pushdown_complex_filter && !assigns_ordinality) {
		// for the remaining filters, check if we can push any of them into the scan as well
		vector<unique_ptr<Expression>> expressions;
		expressions.reserve(filters.size());
		for (auto &filter : filters) {
			expressions.push_back(std::move(filter->filter));
		}
		filters.clear();

		get.function.pushdown_complex_filter(optimizer.context, get, get.bind_data.get(), expressions);

		if (expressions.empty()) {
			restore_barrier_filters();
			return PushFinalFilters(std::move(op));
		}
		// re-generate the filters
		for (auto &expr : expressions) {
			auto f = make_uniq<Filter>();
			f->filter = std::move(expr);
			f->ExtractBindings();
			filters.push_back(std::move(f));
		}
	}
	// Partial type-based filter pushdown is not implemented for table in-out functions.
	const bool requires_partial_pushdown = !get.children.empty() && get.function.supports_pushdown_type;
	// WITH ORDINALITY numbers the rows the function emits, so pushing a filter into the function would renumber the
	// surviving rows rather than report their original positions
	if (get.table_filters.HasFilters() || !get.function.filter_pushdown || requires_partial_pushdown ||
	    assigns_ordinality) {
		// these filters cannot be pushed into the scan: push a LogicalFilter on top
		restore_barrier_filters();
		return FinishPushdown(std::move(op));
	}
	if (PushFilters() == FilterResult::UNSATISFIABLE) {
		return make_uniq<LogicalEmptyResult>(std::move(op));
	}

	auto &column_ids = get.GetColumnIds();
	//! We generate the table filters that will be executed during the table scan
	vector<FilterPushdownResult> pushdown_results;
	get.table_filters = combiner.GenerateTableScanFilters(column_ids, pushdown_results);

	GenerateFilters();

	for (idx_t i = pushdown_results.size(); i < filters.size(); ++i) {
		// any generated filters have not been pushed down yet
		pushdown_results.push_back(FilterPushdownResult::NO_PUSHDOWN);
	}
	// for any filters we did not manage to push into specialized table filters - try to push them as a generic
	// expression
	for (idx_t i = 0; i < filters.size(); ++i) {
		// get the previous pushdown result
		auto pushdown_result = pushdown_results[i];
		if (pushdown_result != FilterPushdownResult::NO_PUSHDOWN) {
			// this has already been (partially) pushed down - skip
			continue;
		}
		auto &expr = *filters[i]->filter;
		if (expr.IsVolatile()) {
			continue;
		}
		// Keep expressions owned by InClauseRewriter in the logical plan so they can become hash joins.
		// Also skip throwing IN expressions: scan pushdown loses short-circuit evaluation semantics.
		if (expr.GetExpressionType() == ExpressionType::COMPARE_IN &&
		    (expr.CanThrow() || InClauseRewriter::HasRewritableInClause(expr))) {
			continue;
		}
		// Allow pushing down filters that can throw only if there is a single expression
		if (expr.CanThrow() && filters.size() > 1) {
			continue;
		}
		pushdown_result = combiner.TryPushdownGenericExpression(get, expr);
		if (pushdown_result == FilterPushdownResult::PUSHED_DOWN_FULLY) {
			filters.erase_at(i);
			pushdown_results.erase_at(i);
			i--;
		}
	}

	PushdownBarrierFilters(get, barrier_filters);
	restore_barrier_filters();

	//! Now we try to pushdown the remaining filters to perform zonemap checking
	return FinishPushdown(std::move(op));
}

} // namespace duckdb
