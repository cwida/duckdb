#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

namespace duckdb {

void CompressedMaterialization::CompressAggregate(unique_ptr<LogicalOperator> &op) {
	auto &aggregate = op->Cast<LogicalAggregate>();
	auto &groups = aggregate.groups;
	column_binding_set_t group_binding_set;
	for (const auto &group : groups) {
		if (group->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		auto &colref = group->Cast<BoundColumnRefExpression>();
		if (group_binding_set.find(colref.Binding()) != group_binding_set.end()) {
			return; // Duplicate group - don't compress
		}
		group_binding_set.insert(colref.Binding());
	}
	auto &group_stats = aggregate.group_stats;

	// No need to compress if there are no groups/stats
	if (groups.empty() || group_stats.empty()) {
		return;
	}
	D_ASSERT(groups.size() == group_stats.size());

	// Find all bindings referenced by non-colref expressions in the groups
	// These are excluded from compression by projection
	// But we can try to compress the expression directly
	column_binding_set_t referenced_bindings;
	vector<ColumnBinding> group_bindings(groups.size(), ColumnBinding());
	vector<CompressedMaterializationType> materialization_types(groups.size(), CompressedMaterializationType::INVALID);
	vector<unique_ptr<BaseStatistics>> stored_group_stats;
	stored_group_stats.resize(groups.size());
	auto try_compress_group = [&](idx_t group_idx, Expression &group_expr, optional_ptr<BaseStatistics> stats) {
		if (!stats) {
			return false;
		}
		auto compress_expr = GetCompressExpression(group_expr.Copy(), *stats);
		if (!compress_expr) {
			return false;
		}
		materialization_types[group_idx] = compress_expr->materialization_type;
		stored_group_stats[group_idx] = stats->ToUnique();
		groups[group_idx] = std::move(compress_expr->expression);
		group_stats[group_idx] = std::move(compress_expr->stats);
		return true;
	};
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &group_expr = *groups[group_idx];
		if (group_expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &colref = group_expr.Cast<BoundColumnRefExpression>();
			group_bindings[group_idx] = colref.Binding();
			continue; // Will be compressed generically
		}

		// Mark the bindings referenced by the non-colref expression so they won't be modified
		GetReferencedBindings(group_expr, referenced_bindings);

		// The non-colref expression won't be compressed generically, so try to compress it here
		if (try_compress_group(group_idx, group_expr, GetVariantWrapperStats(group_expr))) {
			continue;
		}
		try_compress_group(group_idx, group_expr, group_stats[group_idx].get());
	}

	// Anything referenced in the aggregate functions is also excluded
	for (idx_t expr_idx = 0; expr_idx < aggregate.expressions.size(); expr_idx++) {
		const auto &expr = *aggregate.expressions[expr_idx];
		D_ASSERT(expr.GetExpressionType() == ExpressionType::BOUND_AGGREGATE);
		const auto &aggr_expr = expr.Cast<BoundAggregateExpression>();
		for (const auto &child : aggr_expr.GetChildren()) {
			GetReferencedBindings(*child, referenced_bindings);
		}
		if (aggr_expr.GetFilter()) {
			GetReferencedBindings(*aggr_expr.GetFilter(), referenced_bindings);
		}
		if (aggr_expr.GetOrderBys()) {
			for (const auto &order : aggr_expr.GetOrderBys()->orders) {
				const auto &order_expr = *order.expression;
				if (order_expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
					GetReferencedBindings(order_expr, referenced_bindings);
				}
			}
		}
	}

	// Create info for compression
	CompressedMaterializationInfo info(*op, {0}, referenced_bindings);

	// Columns that the aggregate does not read cannot be referenced above it, so leave them uncompressed
	column_binding_set_t used_bindings;
	for (const auto &group : groups) {
		GetReferencedBindings(*group, used_bindings);
	}
	for (const auto &expr : aggregate.expressions) {
		GetReferencedBindings(*expr, used_bindings);
	}
	auto &child_info = info.child_info[0];
	for (idx_t binding_idx = 0; binding_idx < child_info.bindings_before.size(); binding_idx++) {
		if (used_bindings.find(child_info.bindings_before[binding_idx]) == used_bindings.end()) {
			child_info.can_compress[binding_idx] = false;
		}
	}

	// Create binding mapping
	const auto bindings_out = aggregate.GetColumnBindings();
	const auto &types = aggregate.types;
	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		// Aggregate changes bindings as it has a table idx
		CMBindingInfo binding_info(bindings_out[group_idx], types[group_idx]);
		if (!aggregate.grouping_sets.empty() && group_stats[group_idx]) {
			binding_info.stats = group_stats[group_idx]->ToUnique();
		}
		binding_info.materialization_type = materialization_types[group_idx];
		if (materialization_types[group_idx] != CompressedMaterializationType::INVALID) {
			// Compressed non-generically
			auto entry = info.binding_map.emplace(bindings_out[group_idx], std::move(binding_info));
			entry.first->second.stats = std::move(stored_group_stats[group_idx]);
		} else if (group_bindings[group_idx] != ColumnBinding()) {
			info.binding_map.emplace(group_bindings[group_idx], std::move(binding_info));
		}
	}

	// Now try to compress
	CreateProjections(op, info);

	// Update aggregate statistics
	UpdateAggregateStats(op);
}

void CompressedMaterialization::UpdateAggregateStats(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}

	// Update aggregate group stats if compressed
	auto &compressed_aggregate = op->children[0]->Cast<LogicalAggregate>();
	auto &groups = compressed_aggregate.groups;
	auto &group_stats = compressed_aggregate.group_stats;

	for (idx_t group_idx = 0; group_idx < groups.size(); group_idx++) {
		auto &group_expr = *groups[group_idx];
		if (group_expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		auto &colref = group_expr.Cast<BoundColumnRefExpression>();
		if (!group_stats[group_idx]) {
			continue;
		}
		if (colref.GetReturnType() == group_stats[group_idx]->GetType()) {
			continue;
		}
		auto it = statistics_map.find(colref.Binding());
		if (it != statistics_map.end() && it->second) {
			group_stats[group_idx] = it->second->ToUnique();
		}
	}
}

} // namespace duckdb
