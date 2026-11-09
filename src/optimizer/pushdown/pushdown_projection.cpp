#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

static bool IsVolatile(LogicalProjection &proj, const Expression &expr) {
	bool is_volatile = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		D_ASSERT(colref.Depth() == 0);
		auto &proj_expr = proj.GetExpression(colref.Binding());
		if (proj_expr.IsVolatile()) {
			is_volatile = true;
		}
	});
	return is_volatile;
}

static bool ReferencesComputedExpression(LogicalProjection &proj, const Expression &expr) {
	bool references_computed_expression = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		D_ASSERT(colref.Depth() == 0);
		auto &proj_expr = proj.GetExpression(colref.Binding());
		if (proj_expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			references_computed_expression = true;
		}
	});
	return references_computed_expression;
}

static unique_ptr<Expression> CreateColumnReference(const Expression &expr, ColumnBinding binding) {
	unique_ptr<Expression> result = make_uniq<BoundColumnRefExpression>(expr.GetReturnType(), binding);
	if (!expr.GetAlias().empty()) {
		result->SetAlias(expr.GetAlias());
	}
	return result;
}

static unique_ptr<Expression> ReplaceProjectionBindings(LogicalProjection &proj, unique_ptr<Expression> root_expr) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    root_expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &expr) {
		    D_ASSERT(colref.Depth() == 0);
		    // replace the binding with a copy to the expression at the referenced index
		    auto &proj_expr = proj.GetExpression(colref.Binding());
		    auto copy = proj_expr.Copy();
		    if (!colref.GetAlias().empty()) {
			    copy->SetAlias(colref.GetAlias());
		    }
		    expr = std::move(copy);
	    });
	return root_expr;
}

static void CollectReferencedProjections(LogicalProjection &proj, const Expression &expr,
                                         vector<bool> &referenced_projections) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		D_ASSERT(colref.Depth() == 0);
		D_ASSERT(colref.Binding().table_index == proj.table_index);
		referenced_projections[colref.Binding().column_index] = true;
	});
}

static void ReplaceFilterBindings(LogicalProjection &proj, unique_ptr<Expression> &expr, TableIndex lower_table_index,
                                  const vector<ProjectionIndex> &lower_projection_indexes) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
		    D_ASSERT(colref.Depth() == 0);
		    D_ASSERT(colref.Binding().table_index == proj.table_index);
		    const auto projection_index = lower_projection_indexes[colref.Binding().column_index];
		    D_ASSERT(projection_index.IsValid());
		    colref.BindingMutable() = ColumnBinding(lower_table_index, projection_index);
	    });
}

static void AddPassThroughColumns(unique_ptr<Expression> &expr, TableIndex lower_table_index,
                                  vector<unique_ptr<Expression>> &lower_expressions,
                                  column_binding_map_t<ProjectionIndex> &pass_through_indexes) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &colref_expr) {
		    if (colref.Depth() != 0) {
			    return;
		    }
		    auto entry = pass_through_indexes.find(colref.Binding());
		    if (entry == pass_through_indexes.end()) {
			    const auto projection_index = ProjectionIndex(lower_expressions.size());
			    entry = pass_through_indexes.emplace(colref.Binding(), projection_index).first;
			    lower_expressions.push_back(colref_expr->Copy());
		    }
		    colref.BindingMutable() = ColumnBinding(lower_table_index, entry->second);
	    });
}

unique_ptr<LogicalOperator> FilterPushdown::SplitProjection(unique_ptr<LogicalOperator> op,
                                                            vector<unique_ptr<Expression>> split_expressions) {
	auto &proj = op->Cast<LogicalProjection>();
	vector<bool> referenced_projections(proj.expressions.size(), false);
	for (auto &expr : split_expressions) {
		CollectReferencedProjections(proj, *expr, referenced_projections);
	}
	bool all_projections_referenced = true;
	for (auto referenced : referenced_projections) {
		if (!referenced) {
			all_projections_referenced = false;
			break;
		}
	}
	if (all_projections_referenced) {
		return AddLogicalFilter(std::move(op), std::move(split_expressions));
	}

	const auto lower_table_index = optimizer.binder.GenerateTableIndex();
	vector<ProjectionIndex> lower_projection_indexes(proj.expressions.size());
	vector<unique_ptr<Expression>> lower_expressions;
	for (idx_t projection_index = 0; projection_index < proj.expressions.size(); projection_index++) {
		if (!referenced_projections[projection_index]) {
			continue;
		}
		lower_projection_indexes[projection_index] = ProjectionIndex(lower_expressions.size());
		lower_expressions.push_back(std::move(proj.expressions[projection_index]));
		proj.expressions[projection_index] = CreateColumnReference(
		    *lower_expressions.back(), ColumnBinding(lower_table_index, lower_projection_indexes[projection_index]));
	}

	column_binding_map_t<ProjectionIndex> pass_through_indexes;
	for (idx_t projection_index = 0; projection_index < proj.expressions.size(); projection_index++) {
		if (!referenced_projections[projection_index]) {
			AddPassThroughColumns(proj.expressions[projection_index], lower_table_index, lower_expressions,
			                      pass_through_indexes);
		}
	}
	for (auto &expr : split_expressions) {
		ReplaceFilterBindings(proj, expr, lower_table_index, lower_projection_indexes);
	}

	auto lower_projection = make_uniq<LogicalProjection>(lower_table_index, std::move(lower_expressions));
	if (op->children[0]->has_estimated_cardinality) {
		lower_projection->SetEstimatedCardinality(op->children[0]->estimated_cardinality);
	}
	lower_projection->children.push_back(std::move(op->children[0]));
	op->children[0] = AddLogicalFilter(std::move(lower_projection), std::move(split_expressions));
	return op;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownProjection(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_PROJECTION);
	auto &proj = op->Cast<LogicalProjection>();
	// push filter through logical projection
	// all the BoundColumnRefExpressions in the filter should refer to the LogicalProjection
	// we can rewrite them by replacing those references with the expression of the LogicalProjection node
	FilterPushdown child_pushdown(optimizer, convert_mark_joins, projection_mode);
	// There are some expressions can not be pushed down. We should keep them
	// and add an extra filter operator.
	vector<unique_ptr<Expression>> remain_expressions;
	vector<unique_ptr<Expression>> split_expressions;
	auto stays_on_top = [&](const Filter &f) {
		return IsVolatile(proj, *f.filter) || (f.filter->CanThrow() && !f.has_barrier);
	};
	auto splits_off = [&](const Filter &f) {
		return projection_mode == ProjectionMode::PRESERVE_COMPUTED_EXPRESSIONS &&
		       ReferencesComputedExpression(proj, *f.filter);
	};
	// barred expressions may only cross the projection if every other filter does: a filter kept on top of the
	// projection, or split off below it, would otherwise be evaluated after them
	bool hold_back_barriers = false;
	for (auto &filter : filters) {
		if (!filter->has_barrier && (stays_on_top(*filter) || splits_off(*filter))) {
			hold_back_barriers = true;
		}
	}
	for (auto &filter : filters) {
		auto &f = *filter;
		D_ASSERT(f.bindings.size() <= 1);
		if (stays_on_top(f) || (f.has_barrier && hold_back_barriers)) {
			// Volatile and throwing expressions cannot move across the projection. Expressions carrying a barrier
			// are the exception: a projection does not remove any rows, so pushing them through it does not
			// change which rows they are evaluated on - unless another filter stays behind, see above.
			remain_expressions.push_back(std::move(f.filter));
		} else if (splits_off(f)) {
			// Compute filter-dependent projection expressions once, below the filter.
			split_expressions.push_back(std::move(f.filter));
		} else {
			// rewrite the bindings within this subquery
			f.filter = ReplaceProjectionBindings(proj, std::move(f.filter));
			// add the filter to the child pushdown
			if (child_pushdown.AddFilter(std::move(f.filter)) == FilterResult::UNSATISFIABLE) {
				// filter statically evaluates to false, strip tree
				return make_uniq<LogicalEmptyResult>(std::move(op));
			}
		}
	}
	child_pushdown.GenerateFilters();
	// now push into children
	op->children[0] = child_pushdown.Rewrite(std::move(op->children[0]));
	if (op->children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
		// child returns an empty result: generate an empty result here too
		return make_uniq<LogicalEmptyResult>(std::move(op));
	}
	if (!split_expressions.empty()) {
		op = SplitProjection(std::move(op), std::move(split_expressions));
	}
	return AddLogicalFilter(std::move(op), std::move(remain_expressions));
}

} // namespace duckdb
