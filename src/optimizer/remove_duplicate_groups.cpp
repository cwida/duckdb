#include "duckdb/optimizer/remove_duplicate_groups.hpp"

#include "duckdb/common/type_visitor.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

RemoveDuplicateGroups::RemoveDuplicateGroups(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

namespace {

// derived_expr null for same-binding duplicates, set for FD-derived
struct GroupRemoval {
	ProjectionIndex removed_idx;
	ProjectionIndex target_idx;
	unique_ptr<Expression> derived_expr;
};

// Stops once a second distinct binding is seen.
bool CollectColumnRefBindings(const Expression &expr, column_binding_set_t &result) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		result.insert(expr.Cast<BoundColumnRefExpression>().Binding());
		return result.size() <= 1;
	}
	bool ok = true;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (ok && !CollectColumnRefBindings(child, result)) {
			ok = false;
		}
	});
	return ok;
}

// Bases where SQL `=` doesn't imply bit-equality are unsafe: 0e0 == -0e0 (signbit distinguishes),
// '1 day' == '24 hour' (datepart distinguishes). INTERVAL today gets wrapped in
// normalized_interval(...) by the binder, so it shouldn't reach here, but we exclude it
// defensively in case that wrapping ever changes.
bool BaseTypeAllowsCollapse(const LogicalType &type) {
	return !TypeVisitor::Contains(type, [](const LogicalType &t) {
		switch (t.id()) {
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::INTERVAL:
			return true;
		default:
			return false;
		}
	});
}

// Derived groups must reference a single bare column-ref base; non-colref bases would need a
// per-derived injectivity proof.
void CollectGroupRemovals(const LogicalAggregate &aggr, vector<GroupRemoval> &removals, bool &any_derived) {
	const auto &groups = aggr.groups;

	column_binding_map_t<ProjectionIndex> first_occurrence;
	for (auto group_idx : ProjectionIndex::GetIndexes(groups.size())) {
		const auto &group = groups[group_idx];
		if (group->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		const auto &binding = group->Cast<BoundColumnRefExpression>().Binding();
		auto it = first_occurrence.find(binding);
		if (it == first_occurrence.end()) {
			first_occurrence.emplace(binding, group_idx);
		} else {
			removals.push_back({group_idx, it->second, nullptr});
		}
	}

	for (auto group_idx : ProjectionIndex::GetIndexes(groups.size())) {
		const auto &group = groups[group_idx];
		if (group->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		if (group->IsFoldable() || !group->IsConsistent()) {
			continue;
		}
		column_binding_set_t colrefs;
		if (!CollectColumnRefBindings(*group, colrefs) || colrefs.size() != 1) {
			continue;
		}
		auto base_it = first_occurrence.find(*colrefs.begin());
		if (base_it == first_occurrence.end()) {
			continue;
		}
		if (!BaseTypeAllowsCollapse(groups[base_it->second.GetIndex()]->GetReturnType())) {
			continue;
		}
		removals.push_back({group_idx, base_it->second, nullptr});
		any_derived = true;
	}
}

// Compacts survivors, fixes grouping sets, and fills group_binding_map with the pre/post-shift
// mapping. Derived expressions move into removals[*].derived_expr (caller rebinds them above the
// aggregate); same-binding duplicates move into stored_expressions.
void ApplyGroupRemovals(LogicalAggregate &aggr, vector<GroupRemoval> &removals,
                        vector<unique_ptr<Expression>> &stored_expressions,
                        column_binding_map_t<ColumnBinding> &group_binding_map) {
	auto &groups = aggr.groups;
	const idx_t num_original_groups = groups.size();

	vector<bool> removed_flag(num_original_groups, false);
	for (auto &r : removals) {
		auto &expr = groups[r.removed_idx];
		if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			r.derived_expr = std::move(expr);
		} else {
			stored_expressions.emplace_back(std::move(expr));
		}
		removed_flag[r.removed_idx.GetIndex()] = true;
	}

	// Compact survivors and record their post-shift indices.
	vector<idx_t> old_to_post(num_original_groups, 0);
	idx_t write_pos = 0;
	for (idx_t i = 0; i < num_original_groups; i++) {
		if (!removed_flag[i]) {
			old_to_post[i] = write_pos;
			if (write_pos != i) {
				groups[write_pos] = std::move(groups[i]);
			}
			write_pos++;
		}
	}
	groups.resize(write_pos);
	// Removed slots inherit their target's post-shift index.
	for (auto &r : removals) {
		old_to_post[r.removed_idx.GetIndex()] = old_to_post[r.target_idx.GetIndex()];
	}

	for (auto &grouping_set : aggr.grouping_sets) {
		GroupingSet new_set;
		for (auto entry : grouping_set) {
			new_set.insert(ProjectionIndex(old_to_post[entry.GetIndex()]));
		}
		grouping_set = std::move(new_set);
	}

	for (idx_t i = 0; i < num_original_groups; i++) {
		ColumnBinding old_b(aggr.group_index, ProjectionIndex(i));
		ColumnBinding new_b(aggr.group_index, ProjectionIndex(old_to_post[i]));
		group_binding_map.emplace(old_b, new_b);
	}
}

// Builds a passthrough projection mirroring the pre-removal aggregate output, with derived slots
// replaced by the rewritten derived expression. Fills derived_remap for the caller's
// ReplaceBinding pass.
unique_ptr<LogicalProjection>
BuildDerivedProjection(LogicalAggregate &aggr, idx_t num_original_groups, idx_t num_aggregate_outputs,
                       TableIndex original_group_index, TableIndex original_aggregate_index,
                       vector<GroupRemoval> &removals, const column_binding_map_t<ColumnBinding> &group_binding_map,
                       Optimizer &optimizer, column_binding_map_t<ColumnBinding> &derived_remap) {
	const auto new_proj_idx = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> select_list;
	select_list.reserve(num_original_groups + num_aggregate_outputs);

	for (auto orig : ProjectionIndex::GetIndexes(num_original_groups)) {
		auto post = group_binding_map.at(ColumnBinding(original_group_index, orig));
		select_list.emplace_back(
		    make_uniq<BoundColumnRefExpression>(aggr.groups[post.column_index]->GetReturnType(), post));
		derived_remap.emplace(ColumnBinding(original_group_index, orig), ColumnBinding(new_proj_idx, orig));
	}
	for (auto k : ProjectionIndex::GetIndexes(num_aggregate_outputs)) {
		select_list.emplace_back(make_uniq<BoundColumnRefExpression>(aggr.expressions[k.GetIndex()]->GetReturnType(),
		                                                             ColumnBinding(original_aggregate_index, k)));
		derived_remap.emplace(ColumnBinding(original_aggregate_index, k),
		                      ColumnBinding(new_proj_idx, ProjectionIndex(num_original_groups + k.GetIndex())));
	}

	// Patch derived slots: replace the passthrough with the rebound derived expression.
	ColumnBindingReplacer rebinder;
	for (auto &r : removals) {
		if (!r.derived_expr) {
			continue;
		}
		auto base_post = group_binding_map.at(ColumnBinding(original_group_index, r.target_idx));
		const auto &base_source = aggr.groups[base_post.column_index]->Cast<BoundColumnRefExpression>().Binding();
		rebinder.replacement_bindings.clear();
		rebinder.replacement_bindings.emplace_back(base_source, base_post);
		rebinder.VisitExpression(&r.derived_expr);
		select_list[r.removed_idx.GetIndex()] = std::move(r.derived_expr);
	}

	return make_uniq<LogicalProjection>(new_proj_idx, std::move(select_list));
}

} // namespace

void RemoveDuplicateGroups::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		VisitAggregate(op.Cast<LogicalAggregate>());
		break;
	default:
		break;
	}
	LogicalOperatorVisitor::VisitOperatorExpressions(op);
	LogicalOperatorVisitor::VisitOperatorChildren(op);

	// Inject pending derived-collapse projections — only here do we own the child unique_ptr slot.
	for (auto &child_slot : op.children) {
		if (child_slot->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			continue;
		}
		auto pp = pending_projections.find(child_slot->Cast<LogicalAggregate>());
		if (pp == pending_projections.end()) {
			continue;
		}
		auto proj = std::move(pp->second);
		pending_projections.erase(pp);
		if (child_slot->has_estimated_cardinality) {
			proj->SetEstimatedCardinality(child_slot->estimated_cardinality);
		}
		proj->children.emplace_back(std::move(child_slot));
		proj->ResolveOperatorTypes();
		child_slot = std::move(proj);
	}
}

void RemoveDuplicateGroups::VisitAggregate(LogicalAggregate &aggr) {
	if (!aggr.grouping_functions.empty()) {
		return;
	}

	// If there are multiple grouping sets (ROLLUP/CUBE), we cannot remove duplicate groups
	// because the position of groups matters semantically in ROLLUP(col1, col2, col3),
	// even if col1 and col3 reference the same column binding (e.g., after join column replacement)
	if (aggr.grouping_sets.size() > 1) {
		return;
	}

	vector<GroupRemoval> removals;
	bool any_derived = false;
	CollectGroupRemovals(aggr, removals, any_derived);
	if (removals.empty()) {
		return;
	}

	// Earlier statistics (e.g. from the filter simplification) no longer match the groups - the statistics
	// propagation that runs after this optimizer computes them again
	aggr.group_stats.clear();

	const idx_t num_original_groups = aggr.groups.size();
	const idx_t num_aggregate_outputs = aggr.expressions.size();
	const TableIndex original_group_index = aggr.group_index;
	const TableIndex original_aggregate_index = aggr.aggregate_index;

	column_binding_map_t<ColumnBinding> group_binding_map;
	ApplyGroupRemovals(aggr, removals, stored_expressions, group_binding_map);

	column_binding_map_t<ColumnBinding> derived_remap;
	if (any_derived) {
		pending_projections.emplace(aggr,
		                            BuildDerivedProjection(aggr, num_original_groups, num_aggregate_outputs,
		                                                   original_group_index, original_aggregate_index, removals,
		                                                   group_binding_map, optimizer, derived_remap));
	}

	// Replace all references to the old group binding with the new group binding.
	for (const auto &[old_binding, new_binding] : any_derived ? derived_remap : group_binding_map) {
		ReplaceBinding(old_binding, new_binding);
	}
}

} // namespace duckdb
