#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

static bool ExpressionsBecomeFilters(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		return true;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		return op.Cast<LogicalJoin>().join_type == JoinType::INNER;
	default:
		return false;
	}
}

void FilterPushdown::CheckMarkToSemi(LogicalOperator &op, const unordered_set<TableIndex> &table_bindings) {
	auto referenced_bindings = table_bindings;
	if (!ExpressionsBecomeFilters(op)) {
		LogicalOperatorVisitor::EnumerateExpressions(
		    static_cast<const LogicalOperator &>(op), [&](const unique_ptr<Expression> *expression) {
			    ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
			        **expression, [&](const BoundColumnRefExpression &column_ref) {
				        referenced_bindings.insert(column_ref.Binding().table_index);
			        });
		    });
	}

	switch (op.type) {
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::MARK) {
			// Duplicate-eliminated correlated subqueries must keep MARK semantics; converting to SEMI can drop
			// correlation for nested RHS shapes (issue #22267).
			join.convert_mark_to_semi = false;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::MARK) {
			break;
		}
		// if an operator above the mark join includes the mark join index,
		// then the mark join cannot be converted to a semi join
		if (referenced_bindings.find(join.mark_index) != referenced_bindings.end()) {
			join.convert_mark_to_semi = false;
		}
		break;
	}
	default:
		break;
	}

	// Recurse into each child with the bindings referenced on this path.
	for (auto &child : op.children) {
		CheckMarkToSemi(*child, referenced_bindings);
	}
}

FilterPushdown::FilterPushdown(Optimizer &optimizer, bool convert_mark_joins, ProjectionMode projection_mode)
    : optimizer(optimizer), combiner(optimizer.context), convert_mark_joins(convert_mark_joins),
      projection_mode(projection_mode) {
}

bool FilterPushdown::BarrierCanPassThrough(LogicalOperatorType type) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		return true;
	default:
		return false;
	}
}

vector<unique_ptr<Expression>> FilterPushdown::ExtractBarrierFilters() {
	vector<unique_ptr<Expression>> result;
	for (idx_t i = 0; i < filters.size(); i++) {
		if (!filters[i]->has_barrier) {
			continue;
		}
		result.push_back(std::move(filters[i]->filter));
		filters.erase_at(i);
		i--;
	}
	return result;
}

unique_ptr<LogicalOperator> FilterPushdown::Rewrite(unique_ptr<LogicalOperator> op) {
	D_ASSERT(!combiner.HasFilters());
	if (!BarrierCanPassThrough(op->type)) {
		// this operator can remove rows - barred expressions must be evaluated on top of it instead of below it
		auto barrier_expressions = ExtractBarrierFilters();
		if (!barrier_expressions.empty()) {
			auto result = Rewrite(std::move(op));
			return AddLogicalFilter(std::move(result), std::move(barrier_expressions));
		}
	}
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		return PushdownAggregate(std::move(op));
	case LogicalOperatorType::LOGICAL_FILTER:
		return PushdownFilter(std::move(op));
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return PushdownCrossProduct(std::move(op));
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		return PushdownJoin(std::move(op));
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return PushdownProjection(std::move(op));
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_UNION:
		return PushdownSetOperation(std::move(op));
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return PushdownDistinct(std::move(op));
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		// we can just push directly through these operations without any rewriting
		op->children[0] = Rewrite(std::move(op->children[0]));
		return op;
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		// we can't push filters into the materialized CTE (LHS), but we do want to recurse into it
		FilterPushdown pushdown(optimizer, convert_mark_joins, projection_mode);
		op->children[0] = pushdown.Rewrite(std::move(op->children[0]));
		// we can push filters into the rest of the query plan (RHS)
		op->children[1] = Rewrite(std::move(op->children[1]));
		return op;
	}
	case LogicalOperatorType::LOGICAL_GET:
		return PushdownGet(std::move(op));
	case LogicalOperatorType::LOGICAL_LIMIT:
		return PushdownLimit(std::move(op));
	case LogicalOperatorType::LOGICAL_WINDOW:
		return PushdownWindow(std::move(op));
	case LogicalOperatorType::LOGICAL_UNNEST:
		return PushdownUnnest(std::move(op));
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		return PushdownSecureView(std::move(op));
	default:
		return FinishPushdown(std::move(op));
	}
}

ClientContext &FilterPushdown::GetContext() {
	return optimizer.GetContext();
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownJoin(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN || op->type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN);
	auto &join = op->Cast<LogicalJoin>();

	unordered_set<TableIndex> left_bindings, right_bindings;
	LogicalJoin::GetTableReferences(*op->children[0], left_bindings);
	LogicalJoin::GetTableReferences(*op->children[1], right_bindings);

	// every branch keeps the join's projection maps on the operator it returns: the join itself when it survives,
	// the re-created join in PushdownInnerJoin, or a filter carrying the map when a side replaces the join
	switch (join.join_type) {
	case JoinType::OUTER:
		return PushdownOuterJoin(std::move(op), left_bindings, right_bindings);
	case JoinType::INNER:
		//	AsOf joins can't push anything into the RHS, so treat it as a left join
		if (op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			return PushdownLeftJoin(std::move(op), left_bindings, right_bindings);
		}
		return PushdownInnerJoin(std::move(op), left_bindings, right_bindings);
	case JoinType::LEFT:
		return PushdownLeftJoin(std::move(op), left_bindings, right_bindings);
	case JoinType::MARK:
		return PushdownMarkJoin(std::move(op), left_bindings, right_bindings);
	case JoinType::SINGLE:
		return PushdownSingleJoin(std::move(op), left_bindings, right_bindings);
	case JoinType::SEMI:
	case JoinType::ANTI:
		return PushdownSemiAntiJoin(std::move(op));
	default:
		// unsupported join type: stop pushing down
		return FinishPushdown(std::move(op));
	}
}
FilterResult FilterPushdown::PushFilters() {
	for (auto &f : filters) {
		auto result = combiner.AddFilter(std::move(f->filter));
		D_ASSERT(result != FilterResult::UNSUPPORTED);
		if (result == FilterResult::UNSATISFIABLE) {
			// one of the filters is unsatisfiable - abort filter pushdown
			return FilterResult::UNSATISFIABLE;
		}
	}
	filters.clear();
	return FilterResult::SUCCESS;
}

FilterResult FilterPushdown::AddFilter(unique_ptr<Expression> expr) {
	if (PushFilters() == FilterResult::UNSATISFIABLE) {
		return FilterResult::UNSATISFIABLE;
	}
	return combiner.AddConjuncts(std::move(expr));
}

void FilterPushdown::GenerateFilters() {
	if (!filters.empty()) {
		D_ASSERT(!combiner.HasFilters());
		return;
	}
	combiner.GenerateFilters([&](unique_ptr<Expression> filter) {
		auto f = make_uniq<Filter>();
		f->filter = std::move(filter);
		f->ExtractBindings();
		filters.push_back(std::move(f));
	});
}

unique_ptr<LogicalOperator> FilterPushdown::AddLogicalFilter(unique_ptr<LogicalOperator> op,
                                                             vector<unique_ptr<Expression>> expressions) {
	if (expressions.empty()) {
		// No left expressions, so needn't to add an extra filter operator.
		return op;
	}
	// barred expressions are evaluated after every other expression in the filter, so that they never run on rows
	// that one of the other expressions removes
	std::stable_partition(expressions.begin(), expressions.end(),
	                      [](const unique_ptr<Expression> &expr) { return !ExpressionBarrier::Contains(*expr); });
	auto filter = make_uniq<LogicalFilter>();
	if (op->has_estimated_cardinality) {
		// set the filter's estimated cardinality as the child op's.
		// if the filter is created during the filter pushdown optimization, the estimated cardinality will be later
		// overridden during the join order optimization to a more accurate one.
		// if the filter is created during the statistics propagation, the estimated cardinality won't be set unless set
		// here. assuming the filters introduced during the statistics propagation have little effect in reducing the
		// cardinality, we adopt the the cardinality of the child. this could be improved by MinMax info from the
		// statistics propagation
		filter->SetEstimatedCardinality(op->estimated_cardinality);
	}
	filter->expressions = std::move(expressions);
	filter->children.push_back(std::move(op));
	return std::move(filter);
}

unique_ptr<LogicalOperator> FilterPushdown::PushFinalFilters(unique_ptr<LogicalOperator> op) {
	vector<unique_ptr<Expression>> expressions;
	for (auto &f : filters) {
		expressions.push_back(std::move(f->filter));
	}

	return AddLogicalFilter(std::move(op), std::move(expressions));
}

unique_ptr<LogicalOperator> FilterPushdown::PushFiltersIntoDelimJoin(unique_ptr<LogicalOperator> op) {
	for (idx_t i = 0; i < filters.size(); i++) {
		auto &f = *filters[i];
		for (auto &child : op->children) {
			FilterPushdown pushdown(optimizer, convert_mark_joins, projection_mode);

			// check if filter bindings can be applied to the child bindings.
			auto child_bindings = child->GetColumnBindings();
			unordered_set<TableIndex> child_bindings_table;
			for (auto &binding : child_bindings) {
				child_bindings_table.insert(binding.table_index);
			}

			// Check if ALL bindings of the filter are present in the child
			bool should_push = true;
			for (auto &binding : f.bindings) {
				if (child_bindings_table.find(binding) == child_bindings_table.end()) {
					should_push = false;
					break;
				}
			}

			if (!should_push) {
				continue;
			}

			// copy the filter
			auto filter_copy = f.filter->Copy();
			if (pushdown.AddFilter(std::move(filter_copy)) == FilterResult::UNSATISFIABLE) {
				return make_uniq<LogicalEmptyResult>(std::move(op));
			}

			// push the filter into the child.
			pushdown.GenerateFilters();
			child = pushdown.Rewrite(std::move(child));

			// Don't push same filter again
			filters.erase_at(i);
			i--;
			break;
		}
	}
	return op;
}

unique_ptr<LogicalOperator> FilterPushdown::FinishPushdown(unique_ptr<LogicalOperator> op) {
	// unhandled type, first perform filter pushdown in its children
	for (auto &child : op->children) {
		FilterPushdown pushdown(optimizer, convert_mark_joins, projection_mode);
		child = pushdown.Rewrite(std::move(child));
	}
	// now push any existing filters
	return PushFinalFilters(std::move(op));
}

void FilterPushdown::Filter::ExtractBindings() {
	bindings.clear();
	LogicalJoin::GetExpressionBindings(*filter, bindings);
	ExtractBarrier();
}

void FilterPushdown::Filter::ExtractBarrier() {
	has_barrier = ExpressionBarrier::Contains(*filter);
}

} // namespace duckdb
