#include "duckdb/optimizer/remove_unused_columns.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/parser/parsed_data/vacuum_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/subquery/column_binding_layout.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include <utility>

namespace duckdb {

static void GatherCTEScans(const TableIndex cte_index, const LogicalOperator &op,
                           unordered_set<TableIndex> &expected_readers) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte_scan = op.Cast<LogicalCTERef>();
		if (cte_scan.cte_index != cte_index) {
			return;
		}
		expected_readers.insert(cte_scan.table_index);
	}
	for (auto &child : op.children) {
		GatherCTEScans(cte_index, *child, expected_readers);
	}
}

RemoveUnusedColumns::RemoveUnusedColumns(Optimizer &optimizer)
    : optimizer(optimizer), binder(optimizer.binder), context(optimizer.context), everything_referenced(true),
      root(*this) {
}

RemoveUnusedColumns::RemoveUnusedColumns(RemoveUnusedColumns &parent, bool is_root)
    : optimizer(parent.optimizer), binder(parent.binder), context(parent.context), everything_referenced(is_root),
      allow_missing_cte_references(parent.allow_missing_cte_references), mode(parent.mode), root(parent.root) {
}

unordered_map<TableIndex, MaterializedCTEInfo> &RemoveUnusedColumns::GetCTEMap() {
	if (!root.root_cte_map) {
		root.root_cte_map = make_uniq<unordered_map<TableIndex, MaterializedCTEInfo>>();
	}
	return *root.root_cte_map;
}

optional_ptr<unordered_map<TableIndex, MaterializedCTEInfo>> RemoveUnusedColumns::TryGetCTEMap() {
	return root.root_cte_map.get();
}

void RemoveUnusedColumns::GatherRecursiveDependencies(unique_ptr<LogicalOperator> &bottom, TableIndex cte_index,
                                                      const unordered_set<ProjectionIndex> &required_columns,
                                                      unordered_set<ProjectionIndex> &recursive_dependencies) {
	auto bindings = bottom->GetColumnBindings();

	RemoveUnusedColumns analysis(optimizer);
	analysis.everything_referenced = false;
	analysis.allow_missing_cte_references = true;
	analysis.mode = RemoveUnusedColumnsMode::ANALYZE;
	analysis.GetCTEMap().insert({cte_index, MaterializedCTEInfo()});
	for (auto column_idx : required_columns) {
		if (column_idx.GetIndex() >= bindings.size()) {
			throw InternalException("Recursive CTE liveness column is out of range");
		}
		analysis.column_references.emplace(bindings[column_idx.GetIndex()], ReferencedColumn());
	}
	analysis.VisitOperator(bottom);

	auto cte_map = analysis.TryGetCTEMap();
	D_ASSERT(cte_map);
	auto cte_entry = cte_map->find(cte_index);
	D_ASSERT(cte_entry != cte_map->end());
	for (auto &entry : cte_entry->second.column_references) {
		recursive_dependencies.insert(entry.first.column_index);
	}
}

bool RemoveUnusedColumns::ComputeRecursiveRequiredColumns(LogicalRecursiveCTE &rec,
                                                          unordered_set<ProjectionIndex> &required_columns) {
	for (auto &entry : column_references) {
		if (entry.first.table_index == rec.table_index) {
			required_columns.insert(entry.first.column_index);
		}
	}
	if (required_columns.empty()) {
		required_columns.insert(ProjectionIndex(0));
	}
	while (true) {
		unordered_set<ProjectionIndex> recursive_dependencies;
		GatherRecursiveDependencies(rec.children[1], rec.table_index, required_columns, recursive_dependencies);
		const auto old_count = required_columns.size();
		required_columns.insert(recursive_dependencies.begin(), recursive_dependencies.end());
		if (required_columns.size() >= rec.column_count) {
			return false;
		}
		if (required_columns.size() == old_count) {
			return true;
		}
	}
}

void RemoveUnusedColumns::ApplyRecursiveProjections(LogicalRecursiveCTE &rec,
                                                    const unordered_set<ProjectionIndex> &required_columns) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	for (auto column_idx : required_columns) {
		column_references.emplace(ColumnBinding(rec.table_index, column_idx), ReferencedColumn());
	}
	vector<idx_t> entries;
	for (idx_t i = 0; i < rec.column_count; i++) {
		entries.push_back(i);
	}
	ClearUnusedExpressions(entries, rec.table_index);
	ApplySecureViewReplacements();

	for (auto &child : rec.children) {
		child->ResolveOperatorTypes();
		auto bindings = child->GetColumnBindings();
		vector<unique_ptr<Expression>> expressions;
		expressions.reserve(entries.size());
		for (auto column_idx : entries) {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(child->types[column_idx], bindings[column_idx]));
		}
		auto projection = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(expressions));
		if (child->has_estimated_cardinality) {
			projection->SetEstimatedCardinality(child->estimated_cardinality);
		}
		projection->children.push_back(std::move(child));
		child = std::move(projection);
	}
	vector<LogicalType> internal_types;
	internal_types.reserve(entries.size());
	for (auto column_idx : entries) {
		internal_types.push_back(rec.internal_types[column_idx]);
	}
	rec.internal_types = std::move(internal_types);
	rec.column_count = entries.size();
}

vector<ReplacementBinding>
RemoveUnusedColumns::RewriteRecursiveCTEReferences(LogicalRecursiveCTE &rec,
                                                   const unordered_set<ProjectionIndex> &required_columns) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	CTERefPruner cte_ref_pruner(rec.table_index, required_columns);
	cte_ref_pruner.VisitOperator(*rec.children[1]);
	auto binding_replacements = std::move(cte_ref_pruner.binding_replacements);
	ColumnBindingReplacer column_binding_replacer;
	column_binding_replacer.replacement_bindings = binding_replacements;
	column_binding_replacer.VisitOperator(*rec.children[1]);
	return binding_replacements;
}

bool RemoveUnusedColumns::TryPruneRecursiveCTE(LogicalRecursiveCTE &rec) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	GetCTEMap().insert({rec.table_index, MaterializedCTEInfo()});
	if (!rec.union_all || !rec.key_targets.empty() || everything_referenced ||
	    !LogicalSubtreeIsRepeatable(*rec.children[0]) || !LogicalSubtreeIsRepeatable(*rec.children[1])) {
		return false;
	}
	unordered_set<ProjectionIndex> required_columns;
	if (!ComputeRecursiveRequiredColumns(rec, required_columns)) {
		return false;
	}
	ApplyRecursiveProjections(rec, required_columns);
	auto reference_replacements = RewriteRecursiveCTEReferences(rec, required_columns);
	for (auto &child : rec.children) {
		RemoveUnusedColumns remove(*this, true);
		remove.VisitOperator(child);
	}
	for (auto &replacement : reference_replacements) {
		root.projection_map_replacements[replacement.old_binding] = {replacement.new_binding};
		RecordSecureViewReplacement(replacement.old_binding, replacement.new_binding);
	}
	ApplySecureViewReplacements();
	return true;
}

idx_t BaseColumnPruner::ReplaceBinding(ColumnBinding current_binding, ColumnBinding new_binding) {
	auto colrefs = column_references.find(current_binding);
	if (colrefs == column_references.end()) {
		return 1;
	}

	auto &col = colrefs->second;
	idx_t created_bindings;
	if (!col.child_columns.empty() && col.supports_pushdown_extract == PushdownExtractSupport::ENABLED) {
		D_ASSERT(!col.unique_paths.empty());
		//! Pushdown extract is supported, so we are potentially creating multiple bindings, 1 for each unique extract
		//! path
		created_bindings = col.unique_paths.size();
	} else {
		//! No pushdown extract, just rewrite the existing bindings
		for (auto &colref_p : col.bindings) {
			auto &colref = colref_p.get();
			D_ASSERT(colref.Binding() == current_binding);
			colref.BindingMutable() = new_binding;
		}
		created_bindings = 1;
	}
	return created_bindings;
}

template <class T>
void RemoveUnusedColumns::ClearUnusedExpressions(vector<T> &list, TableIndex table_idx, bool replace) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	idx_t new_col_idx = 0;
	// entries that are referred to are moved to the front of the list, the rest is erased at the end
	idx_t kept_count = 0;
	for (idx_t col_idx = 0; col_idx < list.size(); col_idx++) {
		auto current_binding = ColumnBinding(table_idx, ProjectionIndex(col_idx));
		if (replace && !root.active_secure_views.empty()) {
			root.secure_view_replacements[current_binding].clear();
		}
		auto entry = column_references.find(current_binding);
		if (entry == column_references.end()) {
			if (replace) {
				root.projection_map_replacements[current_binding].clear();
			}
			// this entry is not referred to, erase it from the set of expressions
			continue;
		}
		if (kept_count != col_idx) {
			list[kept_count] = std::move(list[col_idx]);
		}
		kept_count++;
		if (!replace) {
			continue;
		}
		bool should_replace = false;
		if (col_idx != new_col_idx) {
			should_replace = true;
		}
		if (!entry->second.child_columns.empty() &&
		    entry->second.supports_pushdown_extract == PushdownExtractSupport::ENABLED) {
			//! One or more children of this column are referenced, and pushdown-extract is enabled
			should_replace = true;
		}
		if (should_replace) {
			// column is used but the ColumnBinding has changed because of removed columns
			auto new_binding = ColumnBinding(table_idx, ProjectionIndex(new_col_idx));
			auto created_bindings = ReplaceBinding(current_binding, new_binding);
			auto &map_replacements = root.projection_map_replacements[current_binding];
			map_replacements.clear();
			for (idx_t binding_idx = 0; binding_idx < created_bindings; binding_idx++) {
				map_replacements.emplace_back(table_idx, ProjectionIndex(new_col_idx + binding_idx));
			}
			if (entry->second.child_columns.empty() ||
			    entry->second.supports_pushdown_extract != PushdownExtractSupport::ENABLED) {
				RecordSecureViewReplacement(current_binding, new_binding);
			}
			new_col_idx += created_bindings;
		} else {
			RecordSecureViewReplacement(current_binding, current_binding);
			new_col_idx++;
		}
	}
	list.erase(list.begin() + NumericCast<int64_t>(kept_count), list.end());
}

enum class ProjectionBindingResolution : uint8_t { SELECTED, REPLACEMENT };

static void ResolveProjectionBindings(ColumnBinding binding, const column_binding_set_t &new_bindings,
                                      const column_binding_map_t<vector<ColumnBinding>> &replacements,
                                      column_binding_set_t &active_bindings, vector<ColumnBinding> &resolved_bindings,
                                      ProjectionBindingResolution resolution) {
	if (resolution == ProjectionBindingResolution::REPLACEMENT && new_bindings.find(binding) != new_bindings.end()) {
		resolved_bindings.push_back(binding);
		return;
	}
	auto replacement = replacements.find(binding);
	if (replacement != replacements.end()) {
		if (!active_bindings.insert(binding).second) {
			throw InternalException("Cyclic column pruning replacement for %s", binding.ToString());
		}
		for (auto &new_binding : replacement->second) {
			if (new_binding == binding) {
				if (new_bindings.find(new_binding) != new_bindings.end()) {
					resolved_bindings.push_back(new_binding);
				}
			} else {
				ResolveProjectionBindings(new_binding, new_bindings, replacements, active_bindings, resolved_bindings,
				                          ProjectionBindingResolution::REPLACEMENT);
			}
		}
		active_bindings.erase(binding);
		return;
	}
	if (new_bindings.find(binding) != new_bindings.end()) {
		resolved_bindings.push_back(binding);
	}
}

static void RemapPrunedProjectionMap(LogicalOperator &op, idx_t child_index,
                                     const vector<ColumnBinding> &old_child_bindings,
                                     const column_binding_map_t<vector<ColumnBinding>> &replacements) {
	auto projection_map = LogicalOperatorVisitor::GetProjectionMap(op, child_index);
	D_ASSERT(projection_map);
	vector<ColumnBinding> selected_bindings;
	if (projection_map->empty()) {
		selected_bindings = old_child_bindings;
	} else {
		selected_bindings.reserve(projection_map->size());
		for (auto projection_index : *projection_map) {
			if (projection_index.GetIndex() >= old_child_bindings.size()) {
				throw InternalException("Projection map references a missing child column");
			}
			selected_bindings.push_back(old_child_bindings[projection_index.GetIndex()]);
		}
	}

	auto new_child_bindings = op.children[child_index]->GetColumnBindings();
	column_binding_set_t new_binding_set(new_child_bindings.begin(), new_child_bindings.end());
	vector<ColumnBinding> rewritten_bindings;
	for (auto &binding : selected_bindings) {
		column_binding_set_t active_bindings;
		ResolveProjectionBindings(binding, new_binding_set, replacements, active_bindings, rewritten_bindings,
		                          ProjectionBindingResolution::SELECTED);
	}
	if (rewritten_bindings.empty()) {
		if (new_child_bindings.size() > 1) {
			*projection_map = {ProjectionIndex(0)};
		} else {
			projection_map->clear();
		}
	} else if (rewritten_bindings == new_child_bindings) {
		projection_map->clear();
	} else {
		*projection_map = ColumnBindingLayout(new_child_bindings).CreateProjectionMap(rewritten_bindings);
	}
}

void RemoveUnusedColumns::VisitPrunableChildren(LogicalOperator &op) {
	for (idx_t child_index = 0; child_index < op.children.size(); child_index++) {
		auto old_child_bindings = op.children[child_index]->GetColumnBindings();
		VisitOperator(op.children[child_index]);
		if (op.HasProjectionMap()) {
			RemapPrunedProjectionMap(op, child_index, old_child_bindings, root.projection_map_replacements);
		}
	}
}

static idx_t ReplaceSecureViewSource(unique_ptr<Expression> &expression, const ColumnBinding &binding,
                                     const Expression &source_expression, bool &has_other_binding) {
	if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = expression->Cast<BoundColumnRefExpression>();
		if (column.Binding() != binding || column.Depth() != 0) {
			has_other_binding = true;
			return 0;
		}
		expression = source_expression.Copy();
		return 1;
	}
	idx_t replacements = 0;
	ExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<Expression> &child) {
		replacements += ReplaceSecureViewSource(child, binding, source_expression, has_other_binding);
	});
	return replacements;
}

static unique_ptr<Expression> ComposeSecureViewSource(const Expression &expression, const ColumnBinding &binding,
                                                      const Expression &source_expression) {
	auto result = expression.Copy();
	bool has_other_binding = false;
	auto replacements = ReplaceSecureViewSource(result, binding, source_expression, has_other_binding);
	if (has_other_binding ||
	    (replacements != 1 && expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT)) {
		return nullptr;
	}
	return result;
}

void RemoveUnusedColumns::RecordSecureViewReplacement(ColumnBinding old_binding, ColumnBinding new_binding,
                                                      unique_ptr<Expression> expression) {
	if (root.active_secure_views.empty()) {
		return;
	}
	auto &columns = root.secure_view_replacements[old_binding];
	for (auto &column : columns) {
		if (column.binding == new_binding &&
		    ((!column.expression && !expression) ||
		     (column.expression && expression && column.expression->Equals(*expression)))) {
			return;
		}
	}
	columns.push_back({new_binding, std::move(expression)});
}

void RemoveUnusedColumns::ApplySecureViewReplacements() {
	if (root.secure_view_replacements.empty()) {
		return;
	}
	for (auto &view_ref : root.active_secure_views) {
		auto &view = view_ref.get();
		vector<ColumnBinding> bindings;
		vector<unique_ptr<Expression>> expressions;
		bool valid = true;
		for (idx_t i = 0; valid && i < view.output_bindings.size(); i++) {
			auto binding = view.output_bindings[i];
			auto replacement = root.secure_view_replacements.find(binding);
			if (replacement == root.secure_view_replacements.end()) {
				bindings.push_back(binding);
				expressions.push_back(std::move(view.output_expressions[i]));
				continue;
			}
			for (auto &column : replacement->second) {
				auto source = column.expression
				                  ? ComposeSecureViewSource(*column.expression, binding, *view.output_expressions[i])
				                  : view.output_expressions[i]->Copy();
				if (!source) {
					valid = false;
					break;
				}
				bindings.push_back(column.binding);
				expressions.push_back(std::move(source));
			}
		}
		if (!valid) {
			bindings.clear();
			expressions.clear();
		}
		view.output_bindings = std::move(bindings);
		view.output_expressions = std::move(expressions);
	}
	root.secure_view_replacements.clear();
}

void RemoveUnusedColumns::VisitSecureView(LogicalSecureView &view) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	D_ASSERT(!everything_referenced);
	D_ASSERT(view.children.size() == 1);
	D_ASSERT(view.output_bindings.size() == view.output_expressions.size());

	root.active_secure_views.push_back(view);
	VisitPrunableChildren(view);
	root.active_secure_views.pop_back();
	view.ResolveOperatorTypes();

	auto new_bindings = view.children[0]->GetColumnBindings();
	auto &new_types = view.children[0]->types;
	vector<unique_ptr<Expression>> new_expressions;
	new_expressions.reserve(new_bindings.size());
	for (idx_t output_idx = 0; output_idx < new_bindings.size(); output_idx++) {
		unique_ptr<Expression> source_expression;
		for (idx_t i = 0; i < view.output_bindings.size(); i++) {
			if (view.output_bindings[i] != new_bindings[output_idx] ||
			    !view.output_expressions[i]->GetReturnType().EqualsIncludingCollation(new_types[output_idx])) {
				continue;
			}
			if (source_expression && !source_expression->Equals(*view.output_expressions[i])) {
				view.output_bindings.clear();
				view.output_expressions.clear();
				return;
			}
			source_expression = view.output_expressions[i]->Copy();
		}
		if (!source_expression) {
			view.output_bindings.clear();
			view.output_expressions.clear();
			return;
		}
		new_expressions.push_back(std::move(source_expression));
	}
	view.output_bindings = std::move(new_bindings);
	view.output_expressions = std::move(new_expressions);
}

void RemoveUnusedColumns::VisitOperator(unique_ptr<LogicalOperator> &op_ref) {
	const bool analyze = mode == RemoveUnusedColumnsMode::ANALYZE;
	auto &op = *op_ref;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_WINDOW: {
		auto &window = op.Cast<LogicalWindow>();
		// Preserve volatile evaluation within the window expression, even when its output is unused.
		// Column references do not propagate volatility from their producers.
		if (!everything_referenced) {
			for (idx_t i = 0; i < window.expressions.size(); i++) {
				if (window.expressions[i]->IsVolatile()) {
					column_references.emplace(ColumnBinding(window.window_index, ProjectionIndex(i)),
					                          ReferencedColumn());
				}
			}
		}
		if (analyze) {
			// Windows also pass through child columns, so retain the existing references.
			for (idx_t i = 0; i < window.expressions.size(); i++) {
				auto binding = ColumnBinding(window.window_index, ProjectionIndex(i));
				if (everything_referenced || column_references.find(binding) != column_references.end()) {
					VisitExpression(&window.expressions[i]);
				}
			}
			VisitOperator(window.children[0]);
			return;
		}
		if (!everything_referenced) {
			ClearUnusedExpressions(window.expressions, window.window_index);
			ApplySecureViewReplacements();
		}
		if (window.expressions.empty()) {
			// A window with no remaining outputs preserves its child's rows and bindings.
			auto child = std::move(window.children[0]);
			op_ref = std::move(child);
			VisitOperator(op_ref);
			return;
		}
		VisitOperatorExpressions(window);
		VisitPrunableChildren(window);
		return;
	}
	case LogicalOperatorType::LOGICAL_SECURE_VIEW: {
		auto &view = op.Cast<LogicalSecureView>();
		if (!analyze && !everything_referenced && view.has_source &&
		    view.output_bindings.size() == view.output_expressions.size()) {
			VisitSecureView(view);
			return;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		// aggregate
		auto &aggr = op.Cast<LogicalAggregate>();
		if (analyze) {
			RemoveUnusedColumns remove(*this, everything_referenced);
			for (auto &group : aggr.groups) {
				remove.VisitExpression(&group);
			}
			for (idx_t i = 0; i < aggr.expressions.size(); i++) {
				auto binding = ColumnBinding(aggr.aggregate_index, ProjectionIndex(i));
				if (everything_referenced || column_references.find(binding) != column_references.end()) {
					remove.VisitExpression(&aggr.expressions[i]);
				}
			}
			remove.VisitOperator(op.children[0]);
			return;
		}
		if (!everything_referenced) {
			// FIXME: groups that are not referenced need to stay -> but they don't need to be scanned and output!
			ClearUnusedExpressions(aggr.expressions, aggr.aggregate_index);
			ApplySecureViewReplacements();
			if (aggr.expressions.empty() && aggr.groups.empty()) {
				// removed all expressions from the aggregate: push a COUNT(*)
				auto count_star_fun = GetBuiltinAggregateFunction(context, CountStarFun::Name, {});
				FunctionBinder function_binder(context);
				aggr.expressions.push_back(
				    function_binder.BindAggregateFunction(count_star_fun, {}, nullptr, AggregateType::NON_DISTINCT));
			}
		}

		// then recurse into the children of the aggregate
		// Note: We allow all optimizations (join column replacement, column pruning) to run below ROLLUP
		// The duplicate groups optimizer will be responsible for not breaking ROLLUP by skipping when
		// multiple grouping sets are present
		RemoveUnusedColumns remove(*this, everything_referenced);
		remove.VisitOperatorExpressions(op);
		remove.VisitOperator(op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		if (analyze || everything_referenced) {
			break;
		}
		auto &comp_join = op.Cast<LogicalComparisonJoin>();

		if (comp_join.join_type != JoinType::INNER) {
			break;
		}
		// for inner joins with equality predicates in the form of (X=Y)
		// we can replace any references to the RHS (Y) to references to the LHS (X)
		// this reduces the amount of columns we need to extract from the join hash table
		// (except in the case of floating point numbers which have +0 and -0, equal but different).
		for (auto &cond : comp_join.conditions) {
			if (!cond.IsComparison() || cond.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			if (cond.GetLHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				continue;
			}
			if (cond.GetRHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				continue;
			}
			if (cond.GetLHS().Cast<BoundColumnRefExpression>().GetReturnType().IsFloating()) {
				continue;
			}
			if (cond.GetRHS().Cast<BoundColumnRefExpression>().GetReturnType().IsFloating()) {
				continue;
			}
			// comparison join between two bound column refs
			// we can replace any reference to the RHS (build-side) with a reference to the LHS (probe-side)
			auto &lhs_col = cond.GetLHS().Cast<BoundColumnRefExpression>();
			auto &rhs_col = cond.GetRHS().Cast<BoundColumnRefExpression>();
			// if there are any columns that refer to the RHS,
			auto colrefs = column_references.find(rhs_col.Binding());
			if (colrefs == column_references.end()) {
				continue;
			}
			for (auto &entry : colrefs->second.bindings) {
				auto &colref = entry.get();
				colref.BindingMutable() = lhs_col.Binding();
				AddBinding(colref);
			}
			column_references.erase(rhs_col.Binding());
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		break;
	case LogicalOperatorType::LOGICAL_UNION: {
		auto &setop = op.Cast<LogicalSetOperation>();
		if (analyze) {
			if (!setop.setop_all || everything_referenced) {
				for (auto &child : op.children) {
					RemoveUnusedColumns remove(*this, true);
					remove.VisitOperator(child);
				}
				return;
			}

			vector<ProjectionIndex> entries;
			for (idx_t i = 0; i < setop.column_count; i++) {
				auto column_idx = ProjectionIndex(i);
				if (column_references.find(ColumnBinding(setop.table_index, column_idx)) != column_references.end()) {
					entries.push_back(column_idx);
				}
			}
			if (entries.empty()) {
				entries.emplace_back(0);
			}
			for (auto &child : op.children) {
				auto bindings = child->GetColumnBindings();
				RemoveUnusedColumns remove(*this, false);
				for (auto column_idx : entries) {
					if (column_idx.GetIndex() >= bindings.size()) {
						throw InternalException("UNION liveness column is out of range");
					}
					remove.column_references.emplace(bindings[column_idx.GetIndex()], ReferencedColumn());
				}
				remove.VisitOperator(child);
			}
			return;
		}
		if (setop.setop_all && !everything_referenced) {
			// for UNION we can remove unreferenced columns if union all is used
			// it's possible not all columns are referenced, but unreferenced columns in the union can
			// still have an affect on the result of the union
			vector<idx_t> entries;
			for (idx_t i = 0; i < setop.column_count; i++) {
				entries.push_back(i);
			}
			ClearUnusedExpressions(entries, setop.table_index);
			if (entries.size() >= setop.column_count) {
				ApplySecureViewReplacements();
				// We still need to recurse into the children to populate CTE info, etc.
				for (auto &child : op.children) {
					RemoveUnusedColumns remove(*this, true);
					remove.VisitOperator(child);
				}

				return;
			}
			if (entries.empty()) {
				// no columns referenced: this happens in the case of a COUNT(*)
				// extract the first column
				entries.push_back(0);
				auto binding = ColumnBinding(setop.table_index, ProjectionIndex(0));
				RecordSecureViewReplacement(binding, binding);
			}
			ApplySecureViewReplacements();
			for (idx_t child_idx = 0; child_idx < op.children.size(); child_idx++) {
				RemoveUnusedColumns remove(*this, true);
				auto &child = op.children[child_idx];

				if (entries.size() < setop.column_count) {
					// columns were cleared
					// push a projection under this child that references the required columns of the union
					child->ResolveOperatorTypes();
					auto bindings = child->GetColumnBindings();
					vector<unique_ptr<Expression>> expressions;
					expressions.reserve(entries.size());
					for (auto &column_idx : entries) {
						expressions.push_back(
						    make_uniq<BoundColumnRefExpression>(child->types[column_idx], bindings[column_idx]));
					}
					auto new_projection =
					    make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(expressions));
					if (child->has_estimated_cardinality) {
						new_projection->SetEstimatedCardinality(child->estimated_cardinality);
					}
					new_projection->children.push_back(std::move(child));
					op.children[child_idx] = std::move(new_projection);
				}

				// now visit the child
				remove.VisitOperator(op.children[child_idx]);
			}
			setop.column_count = entries.size();
			return;
		}
		for (auto &child : op.children) {
			RemoveUnusedColumns remove(*this, true);
			remove.VisitOperator(child);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		// for INTERSECT/EXCEPT operations we can't remove anything, just recursively visit the children
		for (auto &child : op.children) {
			RemoveUnusedColumns remove(*this, true);
			remove.VisitOperator(child);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &proj = op.Cast<LogicalProjection>();
		if (analyze) {
			RemoveUnusedColumns remove(*this, false);
			for (idx_t i = 0; i < proj.expressions.size(); i++) {
				auto binding = ColumnBinding(proj.table_index, ProjectionIndex(i));
				if (everything_referenced || column_references.find(binding) != column_references.end()) {
					remove.VisitExpression(&proj.expressions[i]);
				}
			}
			remove.VisitOperator(op.children[0]);
			return;
		}
		if (!everything_referenced) {
			CheckPushdownExtract(op);
			auto old_expression_count = proj.expressions.size();
			ClearUnusedExpressions(proj.expressions, proj.table_index);
			RewriteExpressions(proj, old_expression_count);

			if (proj.expressions.empty()) {
				// nothing references the projected expressions
				// this happens in the case of e.g. EXISTS(SELECT * FROM ...)
				// in this case we only need to project a single constant
				proj.expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(42)));
				auto binding = ColumnBinding(proj.table_index, ProjectionIndex(0));
				RecordSecureViewReplacement(binding, binding, proj.expressions[0]->Copy());
			}
			ApplySecureViewReplacements();
		}
		// then recurse into the children of this projection
		RemoveUnusedColumns remove(*this, false);
		remove.VisitOperatorExpressions(op);
		remove.VisitOperator(op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO: {
		//! When RETURNING is used, a PROJECTION is the top level operator for INSERTS, UPDATES, and DELETES
		//! We still need to project all values from these operators so the projection
		//! on top of them can select from only the table values being inserted.
		//! TODO: Push down the projections from the returning statement
		//! TODO: Be careful because you might be adding expressions when a user returns *
		RemoveUnusedColumns remove(*this, true);
		remove.VisitOperatorExpressions(op);
		remove.VisitOperator(op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_GET: {
		LogicalOperatorVisitor::VisitOperatorExpressions(op);
		if (!analyze) {
			auto &get = op.Cast<LogicalGet>();
			RemoveColumnsFromLogicalGet(get, op_ref);
		}
		if (!op.children.empty()) {
			// Some LOGICAL_GET operators (e.g., table in out functions) may have a
			// child operator. So we recurse into it if it exists.
			RemoveUnusedColumns remove(*this, true);
			remove.VisitOperator(op.children[0]);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_CHUNK_GET: {
		LogicalOperatorVisitor::VisitOperatorExpressions(op);
		if (!analyze) {
			auto &get = op.Cast<LogicalColumnDataGet>();
			RemoveColumnsFromLogicalColumnDataGet(get);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		auto &distinct = op.Cast<LogicalDistinct>();
		if (distinct.distinct_type == DistinctType::DISTINCT_ON) {
			// distinct type references columns that need to be distinct on, so no
			// need to implicitly reference everything.
			break;
		}
		// Preserve all DISTINCT inputs without affecting sibling branches.
		const auto previous_everything_referenced = everything_referenced;
		everything_referenced = true;
		LogicalOperatorVisitor::VisitOperatorExpressions(op);
		VisitPrunableChildren(op);
		everything_referenced = previous_everything_referenced;
		return;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		if (analyze) {
			LogicalOperatorVisitor::VisitOperatorExpressions(op);
			for (auto &child : op.children) {
				RemoveUnusedColumns remove(*this, true);
				remove.VisitOperator(child);
			}
			return;
		}
		auto &rec = op.Cast<LogicalRecursiveCTE>();
		if (!TryPruneRecursiveCTE(rec)) {
			everything_referenced = true;
			break;
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		auto &cte_map_ref = GetCTEMap();
		auto &cte = op.Cast<LogicalCTE>();
		auto &cte_map_entry = cte_map_ref[cte.table_index];

		// Gather all scans of this CTE in the query and mark them as expected readers of this CTE
		GatherCTEScans(cte.table_index, *cte.children[1], cte_map_entry.expected_readers);
		cte_map_entry.everything_referenced = false;
		auto output_bindings = cte.GetColumnBindings();
		bool has_output_references = false;
		for (auto &binding : output_bindings) {
			if (column_references.find(binding) != column_references.end()) {
				has_output_references = true;
				break;
			}
		}

		// A materialized CTE returns the RHS/continuation columns. If this pass is allowed to prune outputs and a
		// parent registered explicit references to those columns, use them to prune the continuation. Respect
		// everything_referenced barriers, e.g. DML operators that rely on a fixed child schema.
		auto prune_rhs_outputs = !everything_referenced && has_output_references;
		RemoveUnusedColumns rhs_child_optimizer(*this, !prune_rhs_outputs);
		if (prune_rhs_outputs) {
			rhs_child_optimizer.column_references.reserve(column_references.size());
			for (auto &entry : column_references) {
				rhs_child_optimizer.column_references.emplace(entry.first, entry.second);
			}
		}
		rhs_child_optimizer.VisitOperator(cte.children[1]);

		unordered_set<ProjectionIndex> referenced_columns_in_rhs;
		for (auto &entry : cte_map_entry.column_references) {
			referenced_columns_in_rhs.insert(entry.first.column_index);
		}

		// If we have seen all readers of this CTE, and not all columns are referenced, we can prune the left-hand side
		// of the CTE. However, if we have not seen all readers, we opt to not prune, because we might miss column
		// references, resulting in incorrect query results.
		auto have_seen_all_readers = cte_map_entry.expected_readers == cte_map_entry.seen_readers;
		if (!have_seen_all_readers) {
			// We did not traverse the entire plan. Something is wrong.
			throw InternalException("CTE pruning did not traverse the entire plan. This is a bug in the optimizer.");
		}

		// A CTE whose own output exposes its readers' bindings has no projection above the RHS. Operators above
		// the CTE may then reference those readers directly, and the rewrite below only covers the RHS, so
		// pruning here could leave those references stale.
		bool readers_visible_in_output = false;
		for (auto &binding : cte.GetColumnBindings()) {
			if (cte_map_entry.expected_readers.find(binding.table_index) != cte_map_entry.expected_readers.end()) {
				readers_visible_in_output = true;
				break;
			}
		}

		// Distinct column indexes here, unlike the per-reader map, so comparing sizes is a valid width check.
		if (cte_map_entry.everything_referenced || readers_visible_in_output ||
		    referenced_columns_in_rhs.size() == cte.children[0]->GetColumnBindings().size()) {
			// Preserve the CTE input without marking unrelated sibling outputs as referenced.
			RemoveUnusedColumns remove(*this, true);
			remove.VisitOperator(cte.children[0]);
			return;
		}
		if (referenced_columns_in_rhs.empty()) {
			// If we have no columns selected just select the first column.
			referenced_columns_in_rhs.emplace(0);
		}
		if (analyze) {
			auto bindings = cte.children[0]->GetColumnBindings();
			RemoveUnusedColumns lhs_child_analyzer(*this, false);
			for (auto column_idx : referenced_columns_in_rhs) {
				if (column_idx.GetIndex() >= bindings.size()) {
					throw InternalException("CTE liveness column is out of range");
				}
				lhs_child_analyzer.column_references.emplace(bindings[column_idx.GetIndex()], ReferencedColumn());
			}
			lhs_child_analyzer.VisitOperator(cte.children[0]);
			return;
		}

		{
			RemoveUnusedColumns lhs_child_optimizer(*this, true);
			// Construct a projection on top of the left-hand side of the CTE
			// that only projects the columns that are referenced in the right-hand side of the CTE
			cte.children[0]->ResolveOperatorTypes();
			auto bindings = cte.children[0]->GetColumnBindings();
			vector<unique_ptr<Expression>> expressions;
			for (idx_t i = 0; i < bindings.size(); i++) {
				if (referenced_columns_in_rhs.find(ProjectionIndex(i)) != referenced_columns_in_rhs.end()) {
					expressions.push_back(make_uniq<BoundColumnRefExpression>(cte.children[0]->types[i], bindings[i]));
				}
			}

			auto projection = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(expressions));
			projection->children.push_back(std::move(cte.children[0]));
			cte.children[0] = std::move(projection);

			lhs_child_optimizer.VisitOperator(cte.children[0]);

			// After pruning the left-hand side of the CTE, we need to rewrite the CTE references to account for the
			// removed columns.
			CTERefPruner cte_ref_pruner(cte.table_index, referenced_columns_in_rhs);
			cte_ref_pruner.VisitOperator(*cte.children[1]);
			for (auto &replacement : cte_ref_pruner.binding_replacements) {
				root.projection_map_replacements[replacement.old_binding] = {replacement.new_binding};
				RecordSecureViewReplacement(replacement.old_binding, replacement.new_binding);
			}

			ApplySecureViewReplacements();

			// We also need to rewrite the column bindings in the right-hand side of the CTE to account for the removed
			// columns on the left-hand side. Conveniently, the CTERefPruner already has the information about which
			// columns were removed, so we can reuse it for the column binding replacement.
			ColumnBindingReplacer column_binding_replacer;
			column_binding_replacer.replacement_bindings = cte_ref_pruner.binding_replacements;
			column_binding_replacer.VisitOperator(*cte.children[1]);
			return;
		}
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		auto &cte_ref = op.Cast<LogicalCTERef>();

		auto cte_info_map = TryGetCTEMap();
		if (!cte_info_map) {
			everything_referenced = true;
			break;
		}
		auto &cte_map_ref = *cte_info_map;
		auto it = cte_map_ref.find(cte_ref.cte_index);
		if (it == cte_map_ref.end()) {
			if (allow_missing_cte_references) {
				break;
			}
			throw InternalException("Could not find CTE definition for CTE reference");
		}
		auto &cte_entry = it->second;
		auto &referenced_columns = cte_entry.column_references;

		// Mark this CTE reference as a seen reader of the CTE
		it->second.seen_readers.insert(cte_ref.table_index);
		for (auto &entry : column_references) {
			if (entry.first.table_index == cte_ref.table_index) {
				referenced_columns.insert(entry);
			}
		}

		// Keyed per reader, so its size counts reader-column pairs, not columns. Coverage is decided at the CTE.
		cte_entry.everything_referenced |= everything_referenced;
		break;
	}
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
	case LogicalOperatorType::LOGICAL_PIVOT: {
		everything_referenced = true;
		break;
	}
	default:
		break;
	}
	LogicalOperatorVisitor::VisitOperatorExpressions(op);
	VisitPrunableChildren(op);

	if (!analyze &&
	    (op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN || op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
	     op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN)) {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		// after removing duplicate columns we may have duplicate join conditions (if the join graph is cyclical)
		vector<JoinCondition> unique_conditions;
		for (auto &cond : comp_join.conditions) {
			bool found = false;
			for (auto &unique_cond : unique_conditions) {
				if (cond.IsComparison() && unique_cond.IsComparison()) {
					if (cond.GetComparisonType() == unique_cond.GetComparisonType() &&
					    cond.GetLHS().Equals(unique_cond.GetLHS()) && cond.GetRHS().Equals(unique_cond.GetRHS())) {
						found = true;
						break;
					}
				} else if (!cond.IsComparison() && !unique_cond.IsComparison()) {
					if (cond.GetJoinExpression().Equals(unique_cond.GetJoinExpression())) {
						found = true;
						break;
					}
				}
			}
			if (!found) {
				unique_conditions.push_back(std::move(cond));
			}
		}
		comp_join.conditions = std::move(unique_conditions);
	}
}

//! returns: found_path, depth of the found path
std::pair<column_index_set::iterator, idx_t> FindShortestMatchingPath(column_index_set &all_paths,
                                                                      const ColumnIndex &full_path) {
	idx_t depth = 0;
	column_index_set::iterator entry;

	ColumnIndex copy;
	if (full_path.HasPrimaryIndex()) {
		copy = ColumnIndex(full_path.GetPrimaryIndex());
	} else {
		copy = ColumnIndex(full_path.GetFieldName());
	}

	reference<const ColumnIndex> path_iter(full_path);
	reference<ColumnIndex> copy_iter(copy);
	while (true) {
		if (path_iter.get().HasType()) {
			copy_iter.get().SetType(path_iter.get().GetType());
		}
		entry = all_paths.find(copy);
		if (entry != all_paths.end()) {
			//! Path found, we're done
			return make_pair(entry, depth);
		}
		if (!path_iter.get().HasChildren()) {
			break;
		}
		path_iter = path_iter.get().GetChildIndex(0);

		ColumnIndex new_child;
		if (path_iter.get().HasPrimaryIndex()) {
			new_child = ColumnIndex(path_iter.get().GetPrimaryIndex());
		} else {
			new_child = ColumnIndex(path_iter.get().GetFieldName());
		}

		copy_iter.get().AddChildIndex(new_child);
		copy_iter = copy_iter.get().GetChildIndex(0);
		depth++;
	}
	return make_pair(all_paths.end(), depth);
}

void RemoveUnusedColumns::WritePushdownExtractColumns(
    ReferencedColumn &col,
    const std::function<ProjectionIndex(const ColumnIndex &extract_path, optional_ptr<const LogicalType> cast_type)>
        &callback) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	//! For each struct extract, replace the expression with a BoundColumnRefExpression
	//! The expression references a binding created for the extracted path, 1 per unique path
	for (auto &struct_extract : col.struct_extracts) {
		//! Replace the struct extract expression at the right depth with a BoundColumnRefExpression
		auto &full_path = struct_extract.extract_path;

		auto res = FindShortestMatchingPath(col.unique_paths, full_path);
		auto entry = res.first;
		auto depth = res.second;
		if (entry == col.unique_paths.end()) {
			throw InternalException("This path wasn't found in the registered paths for this expression at all!?");
		}
		D_ASSERT(struct_extract.components.size() > depth);
		auto &component = struct_extract.components[depth];
		auto &expr = component.cast ? *component.cast : component.extract;

		auto return_type = expr->GetReturnType();

		auto &colref = col.bindings[struct_extract.bindings_idx];
		auto old_binding = colref.get().Binding();
		auto source = root.active_secure_views.empty() ? nullptr : expr->Copy();
		auto colref_copy = colref.get().Copy();
		expr = std::move(colref_copy);
		auto &new_expr = expr->Cast<BoundColumnRefExpression>();
		new_expr.SetReturnType(return_type);

		auto column_index = callback(*entry, component.cast ? &(*component.cast)->GetReturnType() : nullptr);
		new_expr.BindingMutable().column_index = column_index;
		RecordSecureViewReplacement(old_binding, new_expr.Binding(), std::move(source));
	}
}

static unique_ptr<Expression> ConstructStructExtractFromPath(ClientContext &context, unique_ptr<Expression> target,
                                                             const ColumnIndex &path) {
	auto &struct_type = target->GetReturnType();
	D_ASSERT(struct_type.id() == LogicalTypeId::STRUCT);
	reference<const LogicalType> type_iter(struct_type);
	reference<const ColumnIndex> path_iter(path);
	while (true) {
		vector<unique_ptr<Expression>> arguments(2);
		arguments[0] = (std::move(target));
		if (type_iter.get().id() == LogicalTypeId::VARIANT) {
			// a VARIANT field is addressed by key, and is itself a VARIANT
			arguments[1] = make_uniq<BoundConstantExpression>(Value(path_iter.get().GetFieldName()));
			target = BindBuiltinScalarFunction(context, VariantExtractFun::Name, std::move(arguments));
		} else {
			auto child_index = path_iter.get().GetPrimaryIndex();
			auto &child_types = StructType::GetChildTypes(type_iter.get());
			D_ASSERT(child_index < child_types.size());
			auto is_unnamed = StructType::IsUnnamed(type_iter.get());

			type_iter = child_types[child_index].second;

			if (is_unnamed) {
				arguments[1] = make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(child_index + 1)));
			} else {
				arguments[1] = make_uniq<BoundConstantExpression>(Value(child_types[child_index].first));
			}
			// the constant argument selects the key or the index overload of struct_extract
			target = BindBuiltinScalarFunction(context, StructExtractFun::Name, std::move(arguments));
		}
		if (!path_iter.get().HasChildren()) {
			break;
		}
		path_iter = path_iter.get().GetChildIndex(0);
	}
	return target;
}

void RemoveUnusedColumns::RewriteExpressions(LogicalProjection &proj, idx_t expression_count) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	vector<unique_ptr<Expression>> expressions;
	auto &context = this->context;

	column_index_map<ProjectionIndex> new_bindings;
	idx_t expression_idx = 0;
	for (idx_t i = 0; i < expression_count; i++) {
		auto binding = ColumnBinding(proj.table_index, ProjectionIndex(i));
		auto entry = column_references.find(binding);
		if (entry == column_references.end()) {
			//! Already removed by the call to ClearUnusedExpressions
			continue;
		}
		if (entry->second.child_columns.empty() ||
		    entry->second.supports_pushdown_extract != PushdownExtractSupport::ENABLED) {
			expressions.push_back(std::move(proj.expressions[expression_idx++]));
			continue;
		}
		auto &expr = *proj.expressions[expression_idx++];
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		auto original_binding = colref.Binding();
		auto &column_type = expr.GetReturnType();
		idx_t start = expressions.size();
		//! Pushdown Extract is supported, emit a column for every field
		WritePushdownExtractColumns(
		    entry->second,
		    [&](const ColumnIndex &extract_path, optional_ptr<const LogicalType> cast_type) -> ProjectionIndex {
			    ColumnIndex full_path(i);
			    full_path.AddChildIndex(extract_path);
			    auto new_binding_entry = new_bindings.find(full_path);
			    if (new_binding_entry != new_bindings.end()) {
				    // already exists - emit it directly
				    return new_binding_entry->second;
			    }
			    // binding does not exist - create it
			    auto target = make_uniq<BoundColumnRefExpression>(column_type, original_binding);
			    target->SetAlias(expr.GetAlias());
			    auto new_extract = ConstructStructExtractFromPath(context, std::move(target), extract_path);
			    if (cast_type) {
				    auto cast = BoundCastExpression::AddCastToType(context, std::move(new_extract), *cast_type);
				    new_extract = std::move(cast);
			    }

			    auto new_binding_idx = ColumnBinding::PushExpression(expressions, std::move(new_extract));
			    new_bindings.emplace(full_path, new_binding_idx);
			    return new_binding_idx;
		    });
		for (; start < expressions.size(); start++) {
			VisitExpression(&expressions[start]);
		}
	}
	proj.expressions = std::move(expressions);
}

void RemoveUnusedColumns::CheckPushdownExtract(LogicalOperator &op) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		//! For all referenced struct fields, check if the scan supports pushing down the extract
		auto &column_ids = get.GetColumnIds();
		for (auto proj_idx : ProjectionIndex::GetIndexes(column_ids.size())) {
			auto column_binding = ColumnBinding(get.table_index, proj_idx);
			auto entry = column_references.find(column_binding);
			if (entry == column_references.end()) {
				//! Binding is not referenced, skip
				continue;
			}
			auto &col = entry->second;
			if (col.struct_extracts.empty()) {
				//! Either not a struct, or we're not using struct field projection pushdown - skip it
				continue;
			}
			if (col.supports_pushdown_extract == PushdownExtractSupport::DISABLED) {
				//! We're already not using pushdown extract for this column, no need to check with the scan
				continue;
			}
			auto &column_id = get.GetColumnIndex(column_binding);
			if (column_id.IsPushdownExtract()) {
				//! We already pushed down an extract for this column, so a new path is relative to that
				col.supports_pushdown_extract = PushdownExtractSupport::DISABLED;
				continue;
			}
			auto logical_column_index = LogicalIndex(column_id.GetPrimaryIndex());
			if (!get.function.supports_pushdown_extract || get.function.statistics) {
				//! Either 'statistics_extended' needs to be set or 'statistics' needs to be NULL
				col.supports_pushdown_extract = PushdownExtractSupport::DISABLED;
				continue;
			}
			D_ASSERT(get.bind_data);
			if (get.function.supports_pushdown_extract(*get.bind_data, logical_column_index)) {
				col.supports_pushdown_extract = PushdownExtractSupport::ENABLED;
			} else {
				col.supports_pushdown_extract = PushdownExtractSupport::DISABLED;
			}
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &proj = op.Cast<LogicalProjection>();
		for (auto proj_idx : ProjectionIndex::GetIndexes(proj.expressions.size())) {
			auto &expr = *proj.expressions[proj_idx];
			auto record = column_references.find(ColumnBinding(proj.table_index, proj_idx));
			if (record == column_references.end()) {
				//! Not referenced, skip
				continue;
			}
			auto &col = record->second;
			auto &child_columns = col.child_columns;
			if (child_columns.empty()) {
				//! No children of this column are referenced, skip
				continue;
			}
			if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				//! Not a column reference, can't pull up the extract
				continue;
			}
			if (expr.GetReturnType().id() != LogicalTypeId::STRUCT) {
				//! Extract pull up only supported for STRUCT currently
				continue;
			}
			if (col.supports_pushdown_extract == PushdownExtractSupport::DISABLED) {
				//! Already explicitly disabled, don't need to check
				continue;
			}
			col.supports_pushdown_extract = PushdownExtractSupport::ENABLED;
		}
		return;
	}
	default:
		throw InternalException("CheckPushdownExtract not supported for LogicalOperatorType::%s",
		                        EnumUtil::ToString(op.type));
	}
}

void RemoveUnusedColumns::RemoveColumnsFromLogicalColumnDataGet(LogicalColumnDataGet &get) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	if (everything_referenced) {
		return;
	}

	auto column_ids = get.GetColumnIds();
	ClearUnusedExpressions(column_ids, get.table_index);
	ApplySecureViewReplacements();
	get.SetColumnIds(std::move(column_ids));
}

void RemoveUnusedColumns::RemoveColumnsFromLogicalGet(LogicalGet &get, unique_ptr<LogicalOperator> &op_ref) {
	D_ASSERT(mode == RemoveUnusedColumnsMode::APPLY);
	if (everything_referenced) {
		return;
	}
	if (!get.function.projection_pushdown) {
		return;
	}

	//! The existing column ids
	auto old_column_ids = get.GetColumnIds();
	//! The newly written column ids (same size as 'original_ids')
	vector<ColumnIndex> new_column_ids;
	//! The old index that the new one is based on (same size as 'new_column_ids')
	vector<ProjectionIndex> original_ids;
	//! Map of column index to binding, for pushed down struct extracts
	column_index_map<ProjectionIndex> child_map;
	//! Created bindings per original_column (for pushdown extract verification)
	unordered_map<idx_t, idx_t> created_bindings;

	// Create "selection vector" that contains all indices of the old 'column_ids' of the LogicalGet
	//! i.e: This contains all the columns of the table
	vector<ProjectionIndex> proj_sel;
	for (auto proj_idx : ProjectionIndex::GetIndexes(old_column_ids.size())) {
		proj_sel.push_back(proj_idx);
	}
	// Create a copy so we can later check the difference between these two:
	//! 1. The set of filtered ids containing only the columns referenced by the projection expressions (proj_sel)
	//! 2. The set of filtered ids containing columns referenced by either the projection or the filter expressions
	//! (col_sel)
	auto col_sel = proj_sel;
	// Clear unused ids, exclude filter columns that are projected out immediately
	ClearUnusedExpressions(proj_sel, get.table_index, false);

	//! for each filter that was pushed down - convert them into an expression
	vector<unique_ptr<Expression>> filter_expressions;
	for (auto &entry : get.table_filters) {
		auto filter_idx = entry.GetIndex();
		auto &filter = entry.Filter();
		auto &col_id = get.GetColumnIndex(filter_idx);
		auto column_type = get.GetColumnType(col_id);

		ColumnBinding filter_binding(get.table_index, filter_idx);
		auto column_ref = make_uniq<BoundColumnRefExpression>(std::move(column_type), filter_binding);
		//! Convert the filter to an expression, so we can visit it
		auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "RemoveUnusedColumns::VisitGet");
		auto filter_expr = expr_filter.ToExpression(*column_ref);
		if (filter_expr->IsScalar()) {
			filter_expr = std::move(column_ref);
		}
		filter_expressions.push_back(std::move(filter_expr));
	}
	for (auto &filter_expression : filter_expressions) {
		//! Now visit the filter to add to the 'column_references'
		VisitExpression(&filter_expression);
	}
	//! The 'column_references' hold references to these expressions, so they have to stay alive
	vector<unique_ptr<Expression>> multi_filter_columns;
	for (const auto &filter : get.table_filters.GetMultiColumnFilters()) {
		const auto &expression_filter = ExpressionFilter::GetExpressionFilter(*filter, "RemoveUnusedColumns::VisitGet");
		for (const auto &filter_idx : expression_filter.column_indexes) {
			const auto &col_id = get.GetColumnIndex(filter_idx);
			auto column_type = get.GetColumnType(col_id);
			ColumnBinding filter_binding(get.table_index, filter_idx);
			multi_filter_columns.push_back(make_uniq<BoundColumnRefExpression>(std::move(column_type), filter_binding));
		}
	}
	for (auto &column_ref : multi_filter_columns) {
		VisitExpression(&column_ref);
	}

	//! Check with the LogicalGet whether pushdown-extract is supported
	CheckPushdownExtract(get);

	// Clear unused ids, include filter columns that are projected out immediately
	ClearUnusedExpressions(col_sel, get.table_index);

	bool has_pushdown_extract = false;
	// Now set the column ids in the LogicalGet using the "selection vector"
	for (auto &col_sel_idx : col_sel) {
		auto &logical_column_id = old_column_ids[col_sel_idx];
		auto &column_type = get.GetColumnType(logical_column_id);
		auto entry = column_references.find(ColumnBinding(get.table_index, col_sel_idx));
		if (entry == column_references.end()) {
			throw InternalException("RemoveUnusedColumns - could not find referenced column");
		}
		if (entry->second.child_columns.empty() ||
		    entry->second.supports_pushdown_extract != PushdownExtractSupport::ENABLED) {
			original_ids.emplace_back(col_sel_idx);
			if (logical_column_id.IsPushdownExtract()) {
				//! RemoveUnusedColumns is also used by other optimizers,
				//! so we have to deal with this case and preserve the PushdownExtract we created earlier
				D_ASSERT(entry->second.child_columns.empty() ||
				         entry->second.supports_pushdown_extract == PushdownExtractSupport::DISABLED);
				new_column_ids.emplace_back(logical_column_id);
			} else {
				ColumnIndex new_index(logical_column_id.GetPrimaryIndex(), entry->second.child_columns);
				new_column_ids.emplace_back(std::move(new_index));
			}
			continue;
		}
		auto struct_column_index = logical_column_id.GetPrimaryIndex();

		//! Pushdown Extract is supported, emit a column for every field
		WritePushdownExtractColumns(
		    entry->second,
		    [&](const ColumnIndex &extract_path, optional_ptr<const LogicalType> cast_type) -> ProjectionIndex {
			    ColumnIndex new_index(struct_column_index, {extract_path});
			    new_index.SetPushdownExtractType(column_type, cast_type);

			    auto column_binding_index = new_column_ids.size();
			    auto child_entry = child_map.find(new_index);
			    if (child_entry == child_map.end()) {
				    //! Adds the binding for the child only if it doesn't exist yet
				    child_entry = child_map.emplace(new_index, column_binding_index).first;
				    created_bindings[new_index.GetPrimaryIndex()]++;

				    new_column_ids.emplace_back(std::move(new_index));
				    original_ids.emplace_back(col_sel_idx);
			    }
			    has_pushdown_extract = true;
			    return child_entry->second;
		    });
	}
	if (new_column_ids.empty()) {
		// this generally means we are only interested in whether or not anything exists in the table (e.g.
		// EXISTS(SELECT * FROM tbl)) in this case, we just scan the row identifier column as it means we do not
		// need to read any of the columns
		auto any_column = get.GetAnyColumn();
		original_ids.emplace_back(any_column);
		new_column_ids.emplace_back(any_column);
	}
	ApplySecureViewReplacements();
	get.SetColumnIds(std::move(new_column_ids));

	// remap table filters so they point towards the new set of ids
	if (get.table_filters.HasFilters() || get.table_filters.HasMultiColumnFilters()) {
		// Build a mapping from old ProjectionIndex -> new ProjectionIndex using original_ids
		unordered_map<ProjectionIndex, ProjectionIndex> old_to_new_pos;
		old_to_new_pos.reserve(original_ids.size());
		for (idx_t new_pos = 0; new_pos < original_ids.size(); new_pos++) {
			old_to_new_pos[original_ids[new_pos]] = ProjectionIndex(new_pos);
		}
		TableFilterSet remapped_filters;
		for (auto &entry : get.table_filters) {
			auto it = old_to_new_pos.find(entry.GetIndex());
			if (it == old_to_new_pos.end()) {
				throw InternalException("RemoveUnusedColumns: removed a filter column");
			}
			remapped_filters.PushFilter(it->second, entry.TakeFilter());
		}
		for (const auto &filter : get.table_filters.GetMultiColumnFilters()) {
			auto remapped_filter =
			    ExpressionFilter::GetExpressionFilter(*filter, "RemoveUnusedColumns::RemoveColumnsFromLogicalGet")
			        .Copy();
			for (auto &column_index : remapped_filter->column_indexes) {
				auto it = old_to_new_pos.find(column_index);
				if (it == old_to_new_pos.end()) {
					throw InternalException("RemoveUnusedColumns: removed a multi-column filter column");
				}
				column_index = it->second;
			}
			remapped_filters.PushMultiColumnFilter(std::move(remapped_filter));
		}
		get.table_filters = std::move(remapped_filters);
	}

	if (get.function.filter_prune) {
		// Now set the projection cols by matching the "selection vector" that excludes filter columns
		// with the "selection vector" that includes filter columns
		idx_t col_idx = 0;
		get.projection_ids.clear();
		vector<ProjectionIndex> filtered_original_ids;
		//! Find matching indices between the proj_sel and the col_sel
		for (auto to_keep : proj_sel) {
			for (; col_idx < col_sel.size(); col_idx++) {
				if (to_keep == col_sel[col_idx]) {
					filtered_original_ids.push_back(to_keep);
					break;
				}
			}
		}
		col_idx = 0;
		for (auto col : filtered_original_ids) {
			for (; col_idx < original_ids.size(); col_idx++) {
				if (original_ids[col_idx] == col) {
					get.projection_ids.push_back(ProjectionIndex(col_idx));
				} else if (original_ids[col_idx] > col) {
					break;
				}
			}
		}
	}

	if (has_pushdown_extract && !filter_expressions.empty()) {
		// if we have performed pushdown extract and we have filter expressions we might have adjusted the filter
		// expressions remove the table filters and push a filter, then try to re-push the filters with the new set of
		// projections
		auto filter = make_uniq_base<LogicalOperator, LogicalFilter>();
		filter->expressions = std::move(filter_expressions);
		filter->children.push_back(std::move(op_ref));
		get.table_filters.ClearFilters();

		// try to push filters back into the table scan
		FilterPushdown pushdown(optimizer);
		op_ref = pushdown.Rewrite(std::move(filter));
		// if the filters could not be pushed back into the scan, the columns referenced by the
		// remaining filter have to be included in the output of the scan. Since the projection can
		// only prune columns when it projects out fewer columns than are scanned (see
		// TableFunctionInitInput::CanRemoveFilterColumns), we fall back to outputting all columns
		// in scan order
		if (op_ref->type == LogicalOperatorType::LOGICAL_FILTER) {
			get.projection_ids.clear();
		}
		return;
	}
}

CTERefPruner::CTERefPruner(const TableIndex cte_index, const unordered_set<ProjectionIndex> &referenced_columns)
    : cte_index(cte_index), referenced_columns(referenced_columns) {
}

void CTERefPruner::VisitOperator(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte_ref = op.Cast<LogicalCTERef>();
		if (cte_ref.cte_index != cte_index) {
			return;
		}
		// We have to regenerate the chunk_types of the CTE reference to only include the referenced columns/
		// Otherwise, we would run into issues during execution.
		vector<LogicalType> types;
		// We only prune, never reorder, so we can keep track of the new column indices by counting how many columns we
		// skipped.
		idx_t skipped = 0;
		for (idx_t i = 0; i < cte_ref.chunk_types.size(); i++) {
			if (referenced_columns.find(ProjectionIndex(i)) != referenced_columns.end()) {
				// This column is referenced, keep it and add any necessary binding replacements for the skipped columns
				// if necessary.
				types.push_back(cte_ref.chunk_types[i]);
				if (skipped > 0) {
					ColumnBinding source_binding(cte_ref.table_index, ProjectionIndex(i));
					ColumnBinding target_binding(cte_ref.table_index, ProjectionIndex(i - skipped));
					binding_replacements.push_back(ReplacementBinding(source_binding, target_binding));
					projection_map_replacements[source_binding] = {target_binding};
				}
			} else {
				projection_map_replacements[ColumnBinding(cte_ref.table_index, ProjectionIndex(i))].clear();
				skipped++;
			}
		}
		cte_ref.types = std::move(types);
		cte_ref.chunk_types = cte_ref.types;
	}
	for (idx_t child_index = 0; child_index < op.children.size(); child_index++) {
		auto old_child_bindings = op.children[child_index]->GetColumnBindings();
		VisitOperator(*op.children[child_index]);
		if (op.HasProjectionMap()) {
			RemapPrunedProjectionMap(op, child_index, old_child_bindings, projection_map_replacements);
		}
	}
	VisitOperatorExpressions(op);
}

void BaseColumnPruner::SetMode(BaseColumnPrunerMode mode) {
	this->mode = mode;
}

BaseColumnPrunerMode BaseColumnPruner::GetMode() const {
	return mode;
}

bool BaseColumnPruner::HandleStructExtract(unique_ptr<Expression> &expr_p,
                                           optional_ptr<BoundColumnRefExpression> &colref,
                                           reference<ColumnIndex> &path_ref,
                                           vector<ReferencedExtractComponent> &expressions) {
	auto &function = expr_p->Cast<BoundFunctionExpression>();
	auto &child = function.GetChildrenMutable()[0];
	D_ASSERT(child->GetReturnType().id() == LogicalTypeId::STRUCT);
	auto &bind_data = function.BindInfo()->Cast<StructExtractBindData>();
	// struct extract, check if left child is a bound column ref
	if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// column reference - check if it is a struct
		auto &ref = child->Cast<BoundColumnRefExpression>();
		if (ref.GetReturnType().id() != LogicalTypeId::STRUCT) {
			return false;
		}
		colref = &ref;
		auto &path = path_ref.get();
		path.AddChildIndex(ColumnIndex(bind_data.index));
		path_ref = path.GetChildIndex(0);
		expressions.emplace_back(expr_p);
		return true;
	}
	// not a column reference - try to handle this recursively
	if (!HandleExtractRecursive(child, colref, path_ref, expressions)) {
		return false;
	}
	auto &path = path_ref.get();
	path.AddChildIndex(ColumnIndex(bind_data.index));
	path_ref = path.GetChildIndex(0);

	expressions.emplace_back(expr_p);
	return true;
}

bool BaseColumnPruner::HandleVariantExtract(unique_ptr<Expression> &expr_p,
                                            optional_ptr<BoundColumnRefExpression> &colref,
                                            reference<ColumnIndex> &path_ref,
                                            vector<ReferencedExtractComponent> &expressions) {
	auto &function = expr_p->Cast<BoundFunctionExpression>();
	auto &child = function.GetChildrenMutable()[0];
	D_ASSERT(child->GetReturnType().id() == LogicalTypeId::VARIANT);
	auto &bind_data = function.BindInfo()->Cast<VariantExtractBindData>();
	if (bind_data.component.lookup_mode != VariantChildLookupMode::BY_KEY) {
		//! We don't push down variant extract on ARRAY values
		return false;
	}
	// variant extract, check if left child is a bound column ref
	if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// column reference - check if it is a variant
		auto &ref = child->Cast<BoundColumnRefExpression>();
		if (ref.GetReturnType().id() != LogicalTypeId::VARIANT) {
			return false;
		}
		colref = &ref;

		auto &path = path_ref.get();
		path.AddChildIndex(ColumnIndex(bind_data.component.key));
		path_ref = path.GetChildIndex(0);

		expressions.emplace_back(expr_p);
		return true;
	}
	// not a column reference - try to handle this recursively
	if (!HandleExtractRecursive(child, colref, path_ref, expressions)) {
		return false;
	}

	auto &path = path_ref.get();
	path.AddChildIndex(ColumnIndex(bind_data.component.key));
	path_ref = path.GetChildIndex(0);

	expressions.emplace_back(expr_p);
	return true;
}

bool BaseColumnPruner::HandleExtractRecursive(unique_ptr<Expression> &expr_p,
                                              optional_ptr<BoundColumnRefExpression> &colref,
                                              reference<ColumnIndex> &path_ref,
                                              vector<ReferencedExtractComponent> &expressions) {
	auto &expr = *expr_p;
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	if (function.Function().GetName() != "struct_extract_at" && function.Function().GetName() != "struct_extract" &&
	    function.Function().GetName() != "array_extract" && function.Function().GetName() != "variant_extract") {
		return false;
	}
	if (!function.BindInfo()) {
		return false;
	}
	auto &child = function.GetChildrenMutable()[0];
	auto child_type = child->GetReturnType().id();
	switch (child_type) {
	case LogicalTypeId::STRUCT:
		return HandleStructExtract(expr_p, colref, path_ref, expressions);
	case LogicalTypeId::VARIANT:
		return HandleVariantExtract(expr_p, colref, path_ref, expressions);
	default:
		return false;
	}
}

//! A cast pushed into the scan of a shredded VARIANT is applied to the shredded value instead of the VARIANT. For a
//! JSON target the two differ: a VARIANT serializes to JSON, a shredded VARCHAR would be parsed as JSON.
static bool ExtractCastCanBePushedDown(const BoundColumnRefExpression &colref, const Expression &cast) {
	if (colref.GetReturnType().id() != LogicalTypeId::VARIANT) {
		return true;
	}
	return !cast.GetReturnType().IsJSONType();
}

bool BaseColumnPruner::HandleExtractExpression(unique_ptr<Expression> *expression,
                                               optional_ptr<unique_ptr<Expression>> cast_expression) {
	optional_ptr<BoundColumnRefExpression> colref;
	vector<ReferencedExtractComponent> expressions;

	ColumnIndex path(0);
	reference<ColumnIndex> path_ref(path);
	if (!HandleExtractRecursive(*expression, colref, path_ref, expressions)) {
		return false;
	}
	if (cast_expression && !ExtractCastCanBePushedDown(*colref, **cast_expression)) {
		// the cast stays on top of the pushed-down extract
		cast_expression = nullptr;
	}
	if (cast_expression) {
		auto &top_level = expressions.back();
		top_level.cast = cast_expression;
		path_ref.get().SetType((*cast_expression)->GetReturnType());
	}

	AddBinding(*colref, path.GetChildIndex(0), expressions);
	return true;
}

void BaseColumnPruner::MergeChildColumns(vector<ColumnIndex> &current_child_columns, ColumnIndex &new_child_column) {
	if (current_child_columns.empty()) {
		// there's already a reference to the full column - we can't extract only a subfield
		// skip struct projection pushdown
		return;
	}
	// if we are already extract sub-fields, add it (if it is not there yet)
	for (auto &binding : current_child_columns) {
		if (binding.HasPrimaryIndex()) {
			if (!new_child_column.HasPrimaryIndex()) {
				continue;
			}
			if (binding.GetPrimaryIndex() != new_child_column.GetPrimaryIndex()) {
				continue;
			}
		} else {
			if (new_child_column.HasPrimaryIndex()) {
				continue;
			}
			if (binding.GetFieldName() != new_child_column.GetFieldName()) {
				continue;
			}
		}
		// found a match: sub-field is already projected
		// check if we have child columns
		auto &nested_child_columns = binding.GetChildIndexesMutable();
		if (!new_child_column.HasChildren()) {
			// new child is a reference to a full column - clear any existing bindings (if any)
			nested_child_columns.clear();
		} else {
			// new child has a sub-reference - merge recursively
			D_ASSERT(new_child_column.ChildIndexCount() == 1);
			MergeChildColumns(nested_child_columns, new_child_column.GetChildIndex(0));
		}
		return;
	}
	// this child column is not projected yet - add it in
	current_child_columns.push_back(std::move(new_child_column));
}

void BaseColumnPruner::AddBinding(BoundColumnRefExpression &col, ColumnIndex child_column) {
	auto entry = column_references.find(col.Binding());
	if (entry == column_references.end()) {
		// column not referenced yet - add a binding to it entirely
		ReferencedColumn column;
		column.bindings.push_back(col);
		column.child_columns.push_back(std::move(child_column));
		entry = column_references.emplace(make_pair(col.Binding(), std::move(column))).first;
	} else {
		// column reference already exists - check add the binding
		auto &column = entry->second;
		column.bindings.push_back(col);

		MergeChildColumns(column.child_columns, child_column);
	}
	if (mode == BaseColumnPrunerMode::DISABLE_PUSHDOWN_EXTRACT) {
		//! Any child referenced after this mode is set disables PUSHDOWN_EXTRACT
		entry->second.supports_pushdown_extract = PushdownExtractSupport::DISABLED;
	}
}

void ReferencedColumn::AddPath(const ColumnIndex &path) {
	if (child_columns.empty()) {
		//! Full field already referenced, won't use struct field projection pushdown at all
		return;
	}
	path.VerifySinglePath();

	auto res = FindShortestMatchingPath(unique_paths, path);
	auto entry = res.first;
	if (entry != unique_paths.end()) {
		//! The parent path already exists, don't add the new path
		return;
	}

	//! No parent path exists, but child paths could already be added, remove them if they exist
	//! A child is only removed if the new path matches it - a path with different type information does not
	column_index_set new_path_set;
	new_path_set.emplace(path);
	auto it = unique_paths.begin();
	for (; it != unique_paths.end();) {
		auto &unique_path = *it;
		if (unique_path.IsChildPathOf(path) &&
		    FindShortestMatchingPath(new_path_set, unique_path).first != new_path_set.end()) {
			auto current = it;
			it++;
			unique_paths.erase(current);
		} else {
			it++;
		}
	}
	//! Finally add the new path to the map
	unique_paths.emplace(path);
}

void BaseColumnPruner::AddBinding(BoundColumnRefExpression &col, ColumnIndex child_column,
                                  const vector<ReferencedExtractComponent> &parent) {
	AddBinding(col, child_column);
	auto entry = column_references.find(col.Binding());
	if (entry == column_references.end()) {
		throw InternalException("ColumnBinding for the col was somehow not added by the previous step?");
	}
	auto &referenced_column = entry->second;
	//! Save a reference to the top-level struct extract, so we can potentially replace it later
	D_ASSERT(!referenced_column.bindings.empty());

	//! NOTE: this path does not contain the column index of the root,
	//! i.e 's.a' will just be a ColumnIndex with the index of 'a', without children
	referenced_column.AddPath(child_column);
	referenced_column.struct_extracts.emplace_back(parent, referenced_column.bindings.size() - 1,
	                                               std::move(child_column));
}

void BaseColumnPruner::AddBinding(BoundColumnRefExpression &col) {
	auto entry = column_references.find(col.Binding());
	if (entry == column_references.end()) {
		// column not referenced yet - add a binding to it entirely
		column_references[col.Binding()].bindings.push_back(col);
	} else {
		// column reference already exists - add the binding and clear any sub-references
		auto &column = entry->second;
		column.bindings.push_back(col);
		column.child_columns.clear();
	}
}

static bool TryGetCastChild(unique_ptr<Expression> &expr, optional_ptr<unique_ptr<Expression>> &child) {
	if (expr->GetExpressionType() != ExpressionType::OPERATOR_CAST) {
		return false;
	}
	D_ASSERT(BoundCastExpression::IsCast(*expr));
	auto &cast = expr->Cast<BoundFunctionExpression>();
	if (BoundCastExpression::IsTryCast(cast)) {
		return false;
	}
	child = BoundCastExpression::ChildMutable(cast);
	return true;
}

void BaseColumnPruner::VisitExpression(unique_ptr<Expression> *expression) {
	if ((*expression)->GetExpressionType() == ExpressionType::OPERATOR_TRY ||
	    (*expression)->GetExpressionType() == ExpressionType::OPERATOR_HAS_ERROR) {
		const auto was_inside_try = inside_try;
		inside_try = true;
		LogicalOperatorVisitor::VisitExpression(expression);
		inside_try = was_inside_try;
		return;
	}

	//! Check if this is a struct extract wrapped in a cast. TRY_CAST is already
	//! skipped by TryGetCastChild. A throwing CAST under TRY must also stay put:
	//! unused-columns would otherwise emit CAST(struct_extract) in a child
	//! projection and leave TRY wrapping only the already-cast column (#25236).
	optional_ptr<unique_ptr<Expression>> cast_child;
	if (!inside_try && TryGetCastChild(*expression, cast_child)) {
		if (HandleExtractExpression(cast_child.get(), expression)) {
			// already handled
			return;
		}
	}

	//! Check if this is a struct extract
	if (HandleExtractExpression(expression)) {
		// already handled
		return;
	}
	// recurse
	LogicalOperatorVisitor::VisitExpression(expression);
}

unique_ptr<Expression> BaseColumnPruner::VisitReplace(BoundColumnRefExpression &expr,
                                                      unique_ptr<Expression> *expr_ptr) {
	// add a reference to the entire column
	AddBinding(expr);
	return nullptr;
}

unique_ptr<Expression> BaseColumnPruner::VisitReplace(BoundReferenceExpression &expr,
                                                      unique_ptr<Expression> *expr_ptr) {
	// BoundReferenceExpression should not be used here yet, they only belong in the physical plan
	throw InternalException("BoundReferenceExpression should not be used here yet!");
}

} // namespace duckdb
