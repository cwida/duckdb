#include "duckdb/optimizer/column_lifetime_analyzer.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

void ColumnLifetimeAnalyzer::ExtractUnusedColumnBindings(const vector<ColumnBinding> &bindings,
                                                         column_binding_set_t &unused_bindings) {
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (column_references.find(bindings[i]) == column_references.end()) {
			unused_bindings.insert(bindings[i]);
		}
	}
}

void ColumnLifetimeAnalyzer::GenerateProjectionMap(vector<ColumnBinding> bindings,
                                                   column_binding_set_t &unused_bindings,
                                                   vector<ProjectionIndex> &projection_map) {
	projection_map.clear();
	if (unused_bindings.empty()) {
		return;
	}
	// now iterate over the result bindings of the child
	for (idx_t i = 0; i < bindings.size(); i++) {
		// if this binding does not belong to the unused bindings, add it to the projection map
		if (unused_bindings.find(bindings[i]) == unused_bindings.end()) {
			projection_map.emplace_back(i);
		}
	}
	if (projection_map.size() == bindings.size()) {
		projection_map.clear();
	}
}

void ColumnLifetimeAnalyzer::StandardVisitOperator(LogicalOperator &op) {
	VisitOperatorExpressions(op);
	VisitOperatorChildren(op);
}

void ColumnLifetimeAnalyzer::ExtractColumnBindings(const Expression &expr, vector<ColumnBinding> &bindings) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expr, [&](const BoundColumnRefExpression &bound_ref) { bindings.push_back(bound_ref.Binding()); });
}

void ColumnLifetimeAnalyzer::VisitOperator(LogicalOperator &op) {
	Verify(op);
	if (TopN::CanOptimize(op) && op.children[0]->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		// Let's not mess with this, TopN is more important than projection maps
		// TopN does not support a projection map like Order does
		VisitOperatorExpressions(op);                        // Visit LIMIT
		VisitOperatorExpressions(*op.children[0]);           // Visit ORDER
		StandardVisitOperator(*op.children[0]->children[0]); // Recurse into child of ORDER
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		// FIXME: groups that are not referenced can be removed from projection
		// recurse into the children of the aggregate
		ColumnLifetimeAnalyzer analyzer(optimizer, root);
		analyzer.StandardVisitOperator(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		ColumnLifetimeAnalyzer producer(optimizer, root);
		for (auto &binding : op.children[0]->GetColumnBindings()) {
			producer.column_references.insert(binding);
		}
		producer.VisitOperator(*op.children[0]);
		VisitOperator(*op.children[1]);
		return;
	}
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		if (everything_referenced) {
			break;
		}

		// FIXME: for now, we only push into the projection map for equality (hash) joins
		idx_t has_range = 0;
		bool prefer_range_joins = Settings::Get<PreferRangeJoinsSetting>(optimizer.context);
		if (!comp_join.HasEquality(has_range) || prefer_range_joins) {
			return;
		}

		column_binding_set_t lhs_unused;
		column_binding_set_t rhs_unused;
		ExtractUnusedColumnBindings(op.children[0]->GetColumnBindings(), lhs_unused);
		ExtractUnusedColumnBindings(op.children[1]->GetColumnBindings(), rhs_unused);

		StandardVisitOperator(op);

		// then generate the projection map
		if (op.type != LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			// FIXME: left_projection_map in ASOF join
			GenerateProjectionMap(op.children[0]->GetColumnBindings(), lhs_unused, comp_join.left_projection_map);
		}
		GenerateProjectionMap(op.children[1]->GetColumnBindings(), rhs_unused, comp_join.right_projection_map);
		return;
	}
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		//! When RETURNING is used, a PROJECTION is the top level operator for INSERTS, UPDATES, and DELETES
		//! We still need to project all values from these operators so the projection
		//! on top of them can select from only the table values being inserted.
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		// for set operations/recursive CTEs we don't remove anything, just recursively visit the children
		// FIXME: for UNION we can remove unreferenced columns as long as everything_referenced is false (i.e. we
		// encounter a UNION node that is not preceded by a DISTINCT)
		ColumnLifetimeAnalyzer analyzer(optimizer, root, true);
		analyzer.StandardVisitOperator(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		// then recurse into the children of this projection
		ColumnLifetimeAnalyzer analyzer(optimizer, root);
		analyzer.StandardVisitOperator(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		auto &order = op.Cast<LogicalOrder>();
		if (everything_referenced) {
			break;
		}

		column_binding_set_t unused_bindings;
		ExtractUnusedColumnBindings(op.children[0]->GetColumnBindings(), unused_bindings);

		StandardVisitOperator(op);

		GenerateProjectionMap(op.children[0]->GetColumnBindings(), unused_bindings, order.projection_map);
		return;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		if (everything_referenced) {
			break;
		}

		// filter, figure out which columns are not needed after the filter
		column_binding_set_t unused_bindings;
		ExtractUnusedColumnBindings(op.children[0]->GetColumnBindings(), unused_bindings);

		StandardVisitOperator(op);

		// then generate the projection map
		GenerateProjectionMap(op.children[0]->GetColumnBindings(), unused_bindings, filter.projection_map);

		return;
	}
	default:
		break;
	}
	StandardVisitOperator(op);
}

void ColumnLifetimeAnalyzer::Verify(LogicalOperator &op) {
	if (!Settings::Get<DebugVerificationProjectionSetting>(optimizer.context)) {
		return;
	}
	if (everything_referenced) {
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		AddVerificationProjection(op.children[0], join.left_projection_map);
		if (join.join_type != JoinType::MARK) { // Can't mess up the mark_index
			AddVerificationProjection(op.children[1], join.right_projection_map);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		AddVerificationProjection(op.children[0], op.Cast<LogicalOrder>().projection_map);
		break;
	case LogicalOperatorType::LOGICAL_FILTER:
		AddVerificationProjection(op.children[0], op.Cast<LogicalFilter>().projection_map);
		break;
	default:
		break;
	}
}

void ColumnLifetimeAnalyzer::AddVerificationProjection(unique_ptr<LogicalOperator> &child,
                                                       vector<ProjectionIndex> &projection_map) {
	child->ResolveOperatorTypes();
	const auto child_types = child->types;
	const auto child_bindings = child->GetColumnBindings();
	const auto column_count = child_bindings.size();

	// If our child has columns [i, j], we will generate a projection like so [NULL, j, NULL, i, NULL]
	const auto projection_column_count = column_count * 2 + 1;

	// The parent's projection map indexes positions in the child, remap it to the shuffled layout
	for (auto &entry : projection_map) {
		entry = ProjectionIndex(projection_column_count - 2 - entry.GetIndex() * 2);
	}
	vector<unique_ptr<Expression>> expressions;
	expressions.reserve(projection_column_count);

	// First fill with all NULLs
	for (idx_t col_idx = 0; col_idx < projection_column_count; col_idx++) {
		expressions.emplace_back(make_uniq<BoundConstantExpression>(Value(LogicalType::UTINYINT)));
	}

	// Now place the "real" columns in their respective positions, while keeping track of which column becomes which
	const auto table_index = optimizer.binder.GenerateTableIndex();
	ColumnBindingReplacer replacer;
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		const auto &old_binding = child_bindings[col_idx];
		ProjectionIndex new_col_idx(projection_column_count - 2 - col_idx * 2);
		expressions[new_col_idx] = make_uniq<BoundColumnRefExpression>(child_types[col_idx], old_binding);
		replacer.replacement_bindings.emplace_back(old_binding, ColumnBinding(table_index, new_col_idx));
	}

	// Create a projection and swap the operators accordingly
	auto projection = make_uniq<LogicalProjection>(table_index, std::move(expressions));
	if (child->has_estimated_cardinality) {
		projection->SetEstimatedCardinality(child->estimated_cardinality);
	}
	projection->children.emplace_back(std::move(child));
	child = std::move(projection);

	// Replace references to the old binding (higher up in the plan) with references to the new binding
	replacer.stop_operator = child.get();
	replacer.VisitOperator(root);

	// Add new bindings to column_references, else they are considered "unused"
	for (const auto &replacement_binding : replacer.replacement_bindings) {
		if (column_references.find(replacement_binding.old_binding) != column_references.end()) {
			column_references.insert(replacement_binding.new_binding);
		}
	}
}

unique_ptr<Expression> ColumnLifetimeAnalyzer::VisitReplace(BoundColumnRefExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	column_references.insert(expr.Binding());
	return nullptr;
}

unique_ptr<Expression> ColumnLifetimeAnalyzer::VisitReplace(BoundReferenceExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	// BoundReferenceExpression should not be used here yet, they only belong in the physical plan
	throw InternalException("BoundReferenceExpression should not be used here yet!");
}

} // namespace duckdb
