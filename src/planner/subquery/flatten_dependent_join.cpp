#include "duckdb/planner/subquery/flatten_dependent_join.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/planner/subquery/delim_join_cte_rewriter.hpp"
#include "duckdb/planner/subquery/column_binding_layout.hpp"

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/subquery/rewrite_correlated_expressions.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"

#include <algorithm>

namespace duckdb {

static bool HasCTEAccessor(LogicalOperator &op, TableIndex table_index) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		if (op.Cast<LogicalCTERef>().cte_index == table_index) {
			return true;
		}
	}
	for (auto &child : op.children) {
		if (HasCTEAccessor(*child, table_index)) {
			return true;
		}
	}
	return false;
}

FlattenDependentJoins::FlattenDependentJoins(Binder &binder, const CorrelatedColumns &correlated, bool perform_delim,
                                             bool any_join, optional_ptr<FlattenDependentJoins> parent)
    : binder(binder), correlated_columns(correlated), perform_delim(perform_delim), any_join(any_join), parent(parent) {
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto &col = correlated_columns[i];
		ColumnBinding base_binding = col.binding;
		if (parent) {
			auto parent_base = parent->GetCorrelatedBase(col.binding);
			if (parent_base) {
				base_binding = *parent_base;
			}
		}
		correlated_aliases[col.binding] = base_binding;
		delim_types.push_back(col.type);
	}
	if (parent) {
		MergeCorrelatedAliases(*parent);
	}
}

vector<ColumnBinding> FlattenDependentJoins::CreateContiguousState(ColumnBinding base_binding) const {
	vector<ColumnBinding> state;
	state.reserve(correlated_columns.size());
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		state.push_back(ColumnBinding(base_binding.table_index, ProjectionIndex(base_binding.column_index + i)));
	}
	return state;
}

column_binding_map_t<ColumnBinding>
FlattenDependentJoins::GetCurrentBindings(const vector<ColumnBinding> &state) const {
	D_ASSERT(state.size() == correlated_columns.size());
	column_binding_map_t<ColumnBinding> current_binding_map;
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto base_binding = GetCorrelatedBase(correlated_columns[i].binding);
		D_ASSERT(base_binding);
		current_binding_map[*base_binding] = state[i];
	}
	return current_binding_map;
}

void FlattenDependentJoins::RewriteCorrelatedBindings(LogicalOperator &op, const vector<ColumnBinding> &state) {
	if (!correlated_aliases.empty()) {
		// only references to correlated columns are rewritten, without any there is nothing to visit
		RewriteCorrelatedExpressions::Rewrite(op, GetCurrentBindings(state), correlated_aliases);
	}
	InvalidateAccessCache();
}

void FlattenDependentJoins::InvalidateAccessCache() {
	access_cache.clear();
}

FlattenDependentJoins::SubtreeAccess FlattenDependentJoins::GetSubtreeAccess(LogicalOperator &op) const {
	auto entry = access_cache.find(op);
	if (entry != access_cache.end()) {
		return entry->second;
	}
	SubtreeAccess result;
	if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		auto &dependent_join = op.Cast<LogicalDependentJoin>();
		for (auto &column : dependent_join.correlated_columns) {
			if (correlated_aliases.find(column.binding) != correlated_aliases.end()) {
				result.correlated = true;
				break;
			}
		}
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		if (cteref.correlated_columns > 0) {
			auto rec_cte = binder.recursive_ctes.find(cteref.cte_index);
			if (rec_cte != binder.recursive_ctes.end()) {
				auto &cte_corr_cols = rec_cte->second->Cast<LogicalCTE>().correlated_columns;
				auto cte_corr_start = cte_corr_cols.size() - cteref.correlated_columns;
				for (idx_t i = cte_corr_start; i < cte_corr_cols.size(); i++) {
					if (correlated_aliases.find(cte_corr_cols[i].binding) != correlated_aliases.end()) {
						result.correlated = true;
						break;
					}
				}
			}
		}
	}
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr_ptr) {
		result.volatile_expression = result.volatile_expression || (*expr_ptr)->IsVolatile();
		if (result.correlated) {
			return;
		}
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    **expr_ptr, [&](const BoundColumnRefExpression &bound_colref) {
			    result.correlated |= bound_colref.Depth() > 0 &&
			                         correlated_aliases.find(bound_colref.Binding()) != correlated_aliases.end();
		    });
	});
	for (auto &child : op.children) {
		result.Merge(GetSubtreeAccess(*child));
	}
	access_cache.emplace(op, result);
	return result;
}

bool FlattenDependentJoins::RequiresDomain(LogicalOperator &op) const {
	auto access = GetSubtreeAccess(op);
	return access.correlated || (!perform_delim && access.volatile_expression);
}

optional_ptr<const ColumnBinding> FlattenDependentJoins::GetCorrelatedBase(const ColumnBinding &binding) const {
	auto entry = correlated_aliases.find(binding);
	if (entry == correlated_aliases.end()) {
		return nullptr;
	}
	return entry->second;
}

optional_idx FlattenDependentJoins::GetCorrelatedIndex(const ColumnBinding &binding) const {
	auto base_binding = GetCorrelatedBase(binding);
	auto target_binding = base_binding ? *base_binding : binding;
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto local_base = GetCorrelatedBase(correlated_columns[i].binding);
		D_ASSERT(local_base);
		if (*local_base == target_binding) {
			return i;
		}
	}
	return optional_idx();
}

void FlattenDependentJoins::MergeCorrelatedAliases(const FlattenDependentJoins &source) {
	bool changed = false;
	for (auto &entry : source.correlated_aliases) {
		if (!GetCorrelatedIndex(entry.second).IsValid()) {
			continue;
		}
		auto result = correlated_aliases.emplace(entry);
		changed |= result.second;
		if (!result.second) {
			D_ASSERT(result.first->second == entry.second);
		}
	}
	if (changed) {
		InvalidateAccessCache();
	}
}

void FlattenDependentJoins::AddReplacementAliases(const BindingReplacementGraph &replacements) {
	bool changed = false;
	for (auto &replacement : replacements) {
		auto current_binding = replacements.Resolve(replacement.old_binding);
		auto base_binding = GetCorrelatedBase(current_binding);
		if (base_binding) {
			auto entry = correlated_aliases.find(replacement.old_binding);
			if (entry == correlated_aliases.end() || entry->second != *base_binding) {
				correlated_aliases[replacement.old_binding] = *base_binding;
				changed = true;
			}
		}
	}
	if (changed) {
		InvalidateAccessCache();
	}
}

idx_t FlattenDependentJoins::GetDelimKeyIndex(idx_t index) const {
	D_ASSERT(index < (perform_delim ? correlated_columns.size() : 1));
	if (perform_delim) {
		return index;
	}
	auto delim_index = correlated_columns.GetDelimIndex();
	D_ASSERT(delim_index < correlated_columns.size());
	return delim_index;
}

void FlattenDependentJoins::PatchAccessingOperators(LogicalOperator &subtree_root, TableIndex table_index,
                                                    const CorrelatedColumns &correlated_columns) {
	// patch CTE_REF nodes that access the given CTE
	if (subtree_root.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &reader = subtree_root.Cast<LogicalCTERef>();
		if (reader.cte_index == table_index && reader.correlated_columns == 0) {
			for (auto &column : correlated_columns) {
				reader.chunk_types.push_back(column.type);
				reader.bound_columns.emplace_back(column.name);
			}
			reader.correlated_columns += correlated_columns.size();
		}
	}
	// patch DEPENDENT_JOIN nodes that have a child accessing the given CTE
	if (subtree_root.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		auto &join = subtree_root.Cast<LogicalDependentJoin>();
		bool has_cte_ref_child = false;
		for (auto &child : join.children) {
			if (HasCTEAccessor(*child, table_index)) {
				has_cte_ref_child = true;
				break;
			}
		}
		if (has_cte_ref_child) {
			for (auto &column : correlated_columns) {
				bool found = std::find(join.correlated_columns.begin(), join.correlated_columns.end(), column) !=
				             join.correlated_columns.end();
				if (!found) {
					join.correlated_columns.AddColumnToBack(column);
				}
			}
		}
	}
	// recurse into children
	for (auto &child : subtree_root.children) {
		PatchAccessingOperators(*child, table_index, correlated_columns);
	}
}

void FlattenDependentJoins::CreateDelimJoinConditions(vector<JoinCondition> &conditions,
                                                      const CorrelatedColumns &correlated_columns,
                                                      const vector<ColumnBinding> &state, bool perform_delim) {
	D_ASSERT(state.size() == correlated_columns.size());
	auto key_count = perform_delim ? correlated_columns.size() : 1;
	for (idx_t i = 0; i < key_count; i++) {
		auto delim_index = perform_delim ? i : correlated_columns.GetDelimIndex();
		D_ASSERT(delim_index < correlated_columns.size());
		auto &col = correlated_columns[delim_index];
		JoinCondition cond(make_uniq<BoundColumnRefExpression>(col.name, col.type, col.binding),
		                   make_uniq<BoundColumnRefExpression>(col.name, col.type, state[delim_index]),
		                   ExpressionType::COMPARE_NOT_DISTINCT_FROM);
		conditions.push_back(std::move(cond));
	}
}

static void FinalizeAnyJoins(unique_ptr<LogicalOperator> &plan) {
	for (auto &child : plan->children) {
		FinalizeAnyJoins(child);
	}
	LogicalAnyJoin::TrySpecialize(plan);
}

unique_ptr<LogicalOperator> FlattenDependentJoins::DecorrelateIndependent(Binder &binder,
                                                                          unique_ptr<LogicalOperator> plan) {
	CorrelatedColumns correlated;
	FlattenDependentJoins flatten(binder, correlated);
	flatten.DecorrelateSubtree(plan, true, {});
	if (Settings::Get<DelimJoinAsCteSetting>(binder.context)) {
		DelimJoinCTERewriter::Rewrite(binder, plan);
	}
	// ANY joins must retain their general shape until enclosing decorrelation and binding rewrites are complete.
	FinalizeAnyJoins(plan);
	return plan;
}

FlattenDependentJoins::UnnestingState
FlattenDependentJoins::DecorrelateIndependentSubtree(unique_ptr<LogicalOperator> &plan, bool propagate_null_values) {
	CorrelatedColumns correlated;
	FlattenDependentJoins flatten(binder, correlated);
	auto result = flatten.DecorrelateSubtree(plan, propagate_null_values, {});
	MergeCorrelatedAliases(flatten);
	return result;
}

static void RemoveDummyScanBindings(LogicalOperator &op, vector<ColumnBinding> &bindings) {
	if (op.type == LogicalOperatorType::LOGICAL_DUMMY_SCAN) {
		auto dummy_binding = op.GetColumnBindings()[0];
		bindings.erase(std::remove(bindings.begin(), bindings.end(), dummy_binding), bindings.end());
	}
	for (auto &child : op.children) {
		RemoveDummyScanBindings(*child, bindings);
	}
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::DecorrelateSubtree(unique_ptr<LogicalOperator> &plan,
                                                                                bool propagate_null_values,
                                                                                vector<ColumnBinding> state) {
	auto result = DecorrelateSubtreeInternal(plan, propagate_null_values, std::move(state));
	InvalidateAccessCache();
	D_ASSERT(VerifyUnnestingState(*plan, result));
	return result;
}

FlattenDependentJoins::UnnestingState
FlattenDependentJoins::DecorrelateSubtreeInternal(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
                                                  vector<ColumnBinding> state) {
	if (plan->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		auto &op = plan->Cast<LogicalDependentJoin>();
		DependentJoinOutput output;
		auto left_output = ColumnBindingLayout(op.children[0]->GetColumnBindings());
		auto right_output = ColumnBindingLayout(op.children[1]->GetColumnBindings());
		switch (op.join_type) {
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::MARK:
			output.left_payload = left_output.ProjectBindings(op.left_projection_map);
			break;
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			output.right_payload = right_output.ProjectBindings(op.right_projection_map);
			break;
		default:
			output.left_payload = left_output.ProjectBindings(op.left_projection_map);
			output.right_payload = right_output.ProjectBindings(op.right_projection_map);
			break;
		}
		RemoveDummyScanBindings(*op.children[0], output.left_payload);
		RemoveDummyScanBindings(*op.children[1], output.right_payload);
		for (auto &binding : state) {
			auto left_entry = std::find(output.left_payload.begin(), output.left_payload.end(), binding);
			if (left_entry != output.left_payload.end()) {
				output.left_payload.erase(left_entry);
			}
			auto right_entry = std::find(output.right_payload.begin(), output.right_payload.end(), binding);
			if (right_entry != output.right_payload.end()) {
				output.right_payload.erase(right_entry);
			}
		}
		auto left_child_requires_domain = RequiresDomain(*plan->children[0]);
		auto requires_full_rhs_decorrelation = left_child_requires_domain || op.join_type != JoinType::SINGLE;

		// Phase 1: prepare the left side in the current flattener context.
		auto outer_state = PrepareDependentJoinLeft(plan, propagate_null_values, std::move(state));
		// Phase 2: push the active correlated state through the right subtree in a child flattener.
		FlattenDependentJoins child_flatten(binder, op.correlated_columns, op.perform_delim, op.any_join, this);
		child_flatten.AddReplacementAliases(outer_state.replacement_graph);

		unique_ptr<LogicalOperator> detached_cte = nullptr;
		if (plan->children[1]->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			auto &cte_ref = plan->children[1]->Cast<LogicalMaterializedCTE>();
			if (!child_flatten.RequiresDomain(*cte_ref.children[0])) {
				detached_cte = std::move(plan->children[1]);
				plan->children[1] = std::move(detached_cte->children[1]);
				if (requires_full_rhs_decorrelation) {
					auto result = DecorrelateSubtree(plan, propagate_null_values, std::move(outer_state.bindings));
					result.replacement_graph.Merge(outer_state.replacement_graph);
					detached_cte->children[1] = std::move(plan);
					plan = std::move(detached_cte);
					return result;
				}
			}
		}

		auto old_right_bindings = plan->children[1]->GetColumnBindings();
		auto right_state = child_flatten.PushDownCorrelatedNode(plan->children[1], op.propagate_null_values);
		MergeCorrelatedAliases(child_flatten);
		ColumnBindingRewrite::ApplyToChild(plan, 1, std::move(old_right_bindings), right_state.replacement_graph);

		if (!parent && !correlated_columns.empty()) {
			outer_state.bindings = right_state.bindings;
		}

		// Phase 3: finalize the current dependent join using the state produced on the right.
		auto result = FinalizeDependentJoin(plan, std::move(outer_state), right_state, std::move(output));

		if (detached_cte) {
			detached_cte->children[1] = std::move(plan);
			plan = std::move(detached_cte);
		}
		return result;
	}

	UnnestingState result(std::move(state));
	for (idx_t child_index = 0; child_index < plan->children.size(); child_index++) {
		auto old_child_bindings = plan->children[child_index]->GetColumnBindings();
		auto child_state =
		    DecorrelateSubtree(plan->children[child_index], propagate_null_values, std::move(result.bindings));
		ColumnBindingRewrite::ApplyToChild(plan, child_index, std::move(old_child_bindings),
		                                   child_state.replacement_graph);
		result.bindings = child_state.bindings;
		result.replacement_graph.Merge(child_state.replacement_graph);
		RewriteCorrelatedBindings(*plan, result.bindings);
	}
	return result;
}

// General-purpose Row Number Window Builder
static unique_ptr<LogicalWindow> CreateRowNumberWindow(Binder &binder, unique_ptr<LogicalOperator> child,
                                                       TableIndex table_index,
                                                       vector<unique_ptr<Expression>> partitions = {},
                                                       vector<BoundOrderByNode> orders = {}) {
	auto window = make_uniq<LogicalWindow>(table_index);

	auto row_number = GetBuiltinWindowFunction(binder.context, RowNumberFun::Name, {})->Bind(binder.context);
	row_number->PartitionsMutable() = std::move(partitions);
	row_number->OrderByMutable() = std::move(orders);
	row_number->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	row_number->WindowEndMutable() = WindowBoundary::CURRENT_ROW_ROWS;
	row_number->SetAlias("limit_rownum");

	window->expressions.push_back(std::move(row_number));
	window->AddChild(std::move(child));
	return window;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PrepareDependentJoinLeft(unique_ptr<LogicalOperator> &plan,
                                                                                      bool propagate_null_values,
                                                                                      vector<ColumnBinding> state) {
	auto &op = plan->Cast<LogicalDependentJoin>();
	UnnestingState result(std::move(state));
	// If we have a parent, we unnest the left side of the DEPENDENT JOIN in the parent's context.
	if (parent) {
		// only push the dependent join to the left side, if there is correlation.
		if (RequiresDomain(op)) {
			auto old_left_bindings = op.children[0]->GetColumnBindings();
			if (GetSubtreeAccess(*op.children[0]).correlated) {
				result = PushDownCorrelatedNode(op.children[0], propagate_null_values, std::move(result.bindings));
			} else {
				result = AttachDomainToIndependentSubtree(op.children[0], propagate_null_values);
			}
			ColumnBindingRewrite::ApplyToChild(plan, 0, std::move(old_left_bindings), result.replacement_graph);
		} else {
			// There might be unrelated correlation, so we have to traverse the tree
			auto old_left_bindings = op.children[0]->GetColumnBindings();
			auto independent_state = DecorrelateIndependentSubtree(op.children[0]);
			ColumnBindingRewrite::ApplyToChild(plan, 0, std::move(old_left_bindings),
			                                   independent_state.replacement_graph);
			result.replacement_graph.Merge(independent_state.replacement_graph);
		}

		RewriteCorrelatedBindings(op, result.bindings);
	} else {
		auto old_left_bindings = op.children[0]->GetColumnBindings();
		result = DecorrelateSubtree(op.children[0], true, std::move(result.bindings));
		ColumnBindingRewrite::ApplyToChild(plan, 0, std::move(old_left_bindings), result.replacement_graph);
	}

	if (!op.perform_delim) {
		// if we are not performing a delim join, we push a row_number() OVER() window operator on the LHS
		// and perform all duplicate elimination on that row number instead
		const auto &op_col = op.correlated_columns[op.correlated_columns.GetDelimIndex()];
		op.children[0] = CreateRowNumberWindow(binder, std::move(op.children[0]), op_col.binding.table_index);
	}
	return result;
}

static bool ProjectionMapContains(const vector<ProjectionIndex> &projection_map, ProjectionIndex projection_index) {
	for (idx_t i = 0; i < projection_map.size(); i++) {
		if (projection_map[i].IsValid() && projection_map[i].GetIndex() == projection_index.GetIndex()) {
			return true;
		}
	}
	return false;
}

static void AppendStateToProjectionMap(vector<ProjectionIndex> &projection_map, const ColumnBindingLayout &output,
                                       const vector<ColumnBinding> &state) {
	if (projection_map.empty()) {
		return;
	}
	for (auto &binding : state) {
		auto projection_index = output.GetProjectionIndex(binding);
		if (!ProjectionMapContains(projection_map, projection_index)) {
			projection_map.push_back(projection_index);
		}
	}
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownChild(unique_ptr<LogicalOperator> &plan,
                                                                           bool propagate_null_values,
                                                                           vector<ColumnBinding> state,
                                                                           bool rewrite_parent, idx_t child_idx) {
	auto old_child_bindings = plan->children[child_idx]->GetColumnBindings();
	auto result = PushDownCorrelatedNode(plan->children[child_idx], propagate_null_values, std::move(state));
	ColumnBindingRewrite::ApplyToChild(plan, child_idx, std::move(old_child_bindings), result.replacement_graph);
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = plan->Cast<LogicalFilter>();
		AppendStateToProjectionMap(filter.projection_map,
		                           ColumnBindingLayout(plan->children[child_idx]->GetColumnBindings()),
		                           result.bindings);
	}
	if (rewrite_parent) {
		RewriteCorrelatedBindings(*plan, result.bindings);
	}
	return result;
}

vector<optional_idx> FlattenDependentJoins::GetCTERefCorrelatedPositions(const LogicalCTERef &cteref) const {
	vector<optional_idx> positions(correlated_columns.size());
	if (cteref.correlated_columns == 0) {
		return positions;
	}
	auto rec_cte = binder.recursive_ctes.find(cteref.cte_index);
	if (rec_cte == binder.recursive_ctes.end()) {
		throw InternalException(
		    "Correlated CTE reference has no CTE metadata (CTE index: %llu, reference correlated columns: "
		    "%llu, reference columns: %llu, required correlated columns: %llu)",
		    cteref.cte_index.index, cteref.correlated_columns, cteref.chunk_types.size(), correlated_columns.size());
	}
	auto &cte_corr_cols = rec_cte->second->Cast<LogicalCTE>().correlated_columns;
	if (cteref.correlated_columns > cte_corr_cols.size() || cteref.correlated_columns > cteref.chunk_types.size()) {
		throw InternalException("Correlated CTE reference has inconsistent column counts (CTE index: %llu, reference "
		                        "correlated columns: %llu, CTE correlated columns: %llu, reference columns: %llu)",
		                        cteref.cte_index.index, cteref.correlated_columns, cte_corr_cols.size(),
		                        cteref.chunk_types.size());
	}
	// A nested dependent join can request a different order or subset of the CTE's correlated columns.
	// Match their original bindings instead of assuming that both correlated column lists share positions.
	auto cte_ref_offset = cteref.chunk_types.size() - cteref.correlated_columns;
	auto cte_corr_start = cte_corr_cols.size() - cteref.correlated_columns;
	for (idx_t i = 0; i < cteref.correlated_columns; i++) {
		auto correlated_idx = GetCorrelatedIndex(cte_corr_cols[cte_corr_start + i].binding);
		if (correlated_idx.IsValid()) {
			positions[correlated_idx.GetIndex()] = cte_ref_offset + i;
		}
	}
	return positions;
}

void FlattenDependentJoins::AddCTERefJoinConditions(LogicalComparisonJoin &join, const LogicalCTERef &cteref,
                                                    const vector<optional_idx> &positions,
                                                    const vector<ColumnBinding> &state) const {
	for (idx_t i = 0; i < positions.size(); i++) {
		if (!positions[i].IsValid()) {
			continue;
		}
		JoinCondition cond(make_uniq<BoundColumnRefExpression>(
		                       correlated_columns[i].type,
		                       ColumnBinding(cteref.table_index, ProjectionIndex(positions[i].GetIndex()))),
		                   make_uniq<BoundColumnRefExpression>(correlated_columns[i].type, state[i]),
		                   ExpressionType::COMPARE_NOT_DISTINCT_FROM);
		join.conditions.push_back(std::move(cond));
	}
}

void FlattenDependentJoins::AddCorrelatedJoinConditions(LogicalJoin &join, const vector<ColumnBinding> &left_state,
                                                        const vector<ColumnBinding> &right_state) const {
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto left = make_uniq<BoundColumnRefExpression>(correlated_columns[i].type, left_state[i]);
		auto right = make_uniq<BoundColumnRefExpression>(correlated_columns[i].type, right_state[i]);

		if (join.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
		    join.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			auto &comp_join = join.Cast<LogicalComparisonJoin>();
			JoinCondition cond(std::move(left), std::move(right), ExpressionType::COMPARE_NOT_DISTINCT_FROM);
			comp_join.conditions.push_back(std::move(cond));
		} else {
			auto &logical_any_join = join.Cast<LogicalAnyJoin>();
			auto comparison = BoundComparisonExpression::Create(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
			                                                    std::move(left), std::move(right));
			auto conjunction = make_uniq<BoundConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND, std::move(comparison), std::move(logical_any_join.condition));
			logical_any_join.condition = std::move(conjunction);
		}
	}
}

vector<ColumnBinding> FlattenDependentJoins::CreateDelimCrossProduct(unique_ptr<LogicalOperator> &plan,
                                                                     unique_ptr<LogicalOperator> delim_scan,
                                                                     vector<ColumnBinding> state) const {
	auto cross_product = LogicalCrossProduct::Create(std::move(plan), std::move(delim_scan));
	if (cross_product->type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		auto bindings = cross_product->GetColumnBindings();
		state = vector<ColumnBinding>(
		    bindings.begin(),
		    bindings.begin() + NumericCast<vector<ColumnBinding>::difference_type>(correlated_columns.size()));
	}
	plan = std::move(cross_product);
	return state;
}

static bool IsJoinWithProjectionMap(LogicalOperatorType type) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		return true;
	default:
		return false;
	}
}

FlattenDependentJoins::UnnestingState
FlattenDependentJoins::AttachDelimToIndependentJoinLeft(unique_ptr<LogicalOperator> &plan) {
	auto &join = plan->Cast<LogicalJoin>();
	auto &left = plan->children[0];
	auto original_left_output = ColumnBindingLayout(left->GetColumnBindings());
	auto projected_left_bindings = original_left_output.ProjectBindings(join.left_projection_map);
	auto old_left_bindings = left->GetColumnBindings();
	auto result = DecorrelateIndependentSubtree(left);
	ColumnBindingRewrite::ApplyToChild(plan, 0, std::move(old_left_bindings), result.replacement_graph);
	for (auto &binding : projected_left_bindings) {
		binding = result.replacement_graph.Resolve(binding);
	}

	auto delim_index = binder.GenerateTableIndex();
	auto left_state = CreateContiguousState(ColumnBinding(delim_index, ProjectionIndex(0)));
	auto delim_scan = make_uniq<LogicalDelimGet>(delim_index, delim_types);
	left_state = CreateDelimCrossProduct(left, std::move(delim_scan), std::move(left_state));

	auto attached_output = ColumnBindingLayout(left->GetColumnBindings());
	projected_left_bindings.insert(projected_left_bindings.end(), left_state.begin(), left_state.end());
	auto &rewritten_join = plan->Cast<LogicalJoin>();
	rewritten_join.left_projection_map = attached_output.HasSameLayout(projected_left_bindings)
	                                         ? vector<ProjectionIndex>()
	                                         : attached_output.CreateProjectionMap(projected_left_bindings);
	result.bindings = std::move(left_state);
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::FinalizeDependentJoin(unique_ptr<LogicalOperator> &plan,
                                                                                   UnnestingState outer_state,
                                                                                   const UnnestingState &right_state,
                                                                                   DependentJoinOutput output) {
	auto &op = plan->Cast<LogicalDependentJoin>();
	RewriteCorrelatedBindings(op, outer_state.bindings);

	vector<unique_ptr<Expression>> duplicate_eliminated_columns;
	vector<LogicalType> mark_types;
	for (idx_t i = 0; i < op.correlated_columns.size(); i++) {
		auto &col = op.correlated_columns[i];
		duplicate_eliminated_columns.push_back(make_uniq<BoundColumnRefExpression>(col.type, col.binding));
	}
	outer_state.replacement_graph.Merge(right_state.replacement_graph);
	for (auto &binding : output.left_payload) {
		binding = outer_state.replacement_graph.Resolve(binding);
	}
	for (auto &binding : output.right_payload) {
		binding = outer_state.replacement_graph.Resolve(binding);
	}
	// Correlated aggregate rewrites reference marker columns that must survive the projection maps below
	auto keep_rewrite_inputs = [&](vector<ColumnBinding> &payload) {
		for (idx_t i = 0, count = payload.size(); i < count; i++) {
			auto entry = replacement_map.find(payload[i]);
			if (entry == replacement_map.end()) {
				continue;
			}
			for (auto &extra : {entry->second.marker, entry->second.empty_result}) {
				auto resolved = outer_state.replacement_graph.Resolve(extra);
				if (std::find(payload.begin(), payload.end(), resolved) == payload.end()) {
					payload.push_back(resolved);
				}
			}
		}
	};
	keep_rewrite_inputs(output.left_payload);
	keep_rewrite_inputs(output.right_payload);
	for (auto &binding : outer_state.bindings) {
		binding = outer_state.replacement_graph.Resolve(binding);
	}

	auto left_output = ColumnBindingLayout(op.children[0]->GetColumnBindings(), "decorrelated dependent join left");
	auto right_output = ColumnBindingLayout(op.children[1]->GetColumnBindings(), "decorrelated dependent join right");
	auto left_selected = std::move(output.left_payload);
	auto right_selected = std::move(output.right_payload);
	for (auto &binding : outer_state.bindings) {
		auto in_left = left_output.positions.count(binding) > 0;
		auto in_right = right_output.positions.count(binding) > 0;
		if (in_left == in_right) {
			throw InternalException("Dependent-join state binding %s must be owned by exactly one input",
			                        binding.ToString());
		}
		if (in_left) {
			if (op.join_type == JoinType::RIGHT_SEMI || op.join_type == JoinType::RIGHT_ANTI) {
				throw InternalException("Dependent-join state is not emitted by the right-only join output");
			}
			if (std::find(left_selected.begin(), left_selected.end(), binding) == left_selected.end()) {
				left_selected.push_back(binding);
			}
		} else {
			if (op.join_type == JoinType::SEMI || op.join_type == JoinType::ANTI || op.join_type == JoinType::MARK) {
				throw InternalException("Dependent-join state is not emitted by the left-only join output");
			}
			if (std::find(right_selected.begin(), right_selected.end(), binding) == right_selected.end()) {
				right_selected.push_back(binding);
			}
		}
	}
	if (!left_selected.empty()) {
		// Payload and correlation state are binding-addressed here. If the complete child output is selected, keep its
		// physical order and let positional consumers establish their own projection maps.
		op.left_projection_map = left_output.HasSameBindings(left_selected)
		                             ? vector<ProjectionIndex>()
		                             : left_output.CreateProjectionMap(left_selected);
	}
	if (!right_selected.empty()) {
		op.right_projection_map = right_output.HasSameBindings(right_selected)
		                              ? vector<ProjectionIndex>()
		                              : right_output.CreateProjectionMap(right_selected);
	}

	vector<JoinCondition> conditions;
	CreateDelimJoinConditions(conditions, op.correlated_columns, right_state.bindings, op.perform_delim);
	for (auto &condition : conditions) {
		mark_types.push_back(condition.GetLHS().GetReturnType());
	}
	if (op.condition) {
		if (op.any_join) {
			D_ASSERT(BoundComparisonExpression::IsComparison(*op.condition));
			auto comparison_type = op.condition->GetExpressionType();
			auto &comparison = op.condition->Cast<BoundFunctionExpression>();
			conditions.emplace_back(std::move(BoundComparisonExpression::LeftMutable(comparison)),
			                        std::move(BoundComparisonExpression::RightMutable(comparison)), comparison_type);
		} else {
			LogicalComparisonJoin::ExtractJoinConditions(binder.context, op.join_type, JoinRefType::REGULAR,
			                                             op.children[0], op.children[1], std::move(op.condition),
			                                             conditions);
		}
	}

	auto finalized = make_uniq<LogicalComparisonJoin>(op.join_type, LogicalOperatorType::LOGICAL_DELIM_JOIN);
	finalized->conditions = std::move(conditions);
	finalized->mark_types = std::move(mark_types);
	finalized->duplicate_eliminated_columns = std::move(duplicate_eliminated_columns);
	finalized->children = std::move(op.children);
	LogicalJoin::MoveJoinState(op, *finalized);
	plan = std::move(finalized);
	return outer_state;
}

FlattenDependentJoins::UnnestingState
FlattenDependentJoins::PushDownSingleCorrelatedChild(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
                                                     vector<ColumnBinding> state, bool correlated_left) {
	idx_t correlated_idx = correlated_left ? 0 : 1;
	idx_t independent_idx = correlated_left ? 1 : 0;
	auto old_correlated_bindings = plan->children[correlated_idx]->GetColumnBindings();
	auto result = PushDownCorrelatedNode(plan->children[correlated_idx], propagate_null_values, std::move(state));
	ColumnBindingRewrite::ApplyToChild(plan, correlated_idx, std::move(old_correlated_bindings),
	                                   result.replacement_graph);
	RewriteCorrelatedBindings(*plan, result.bindings);
	if (IsJoinWithProjectionMap(plan->type)) {
		auto &join = plan->Cast<LogicalJoin>();
		if (correlated_left) {
			AppendStateToProjectionMap(join.left_projection_map,
			                           ColumnBindingLayout(plan->children[0]->GetColumnBindings()), result.bindings);
		} else {
			AppendStateToProjectionMap(join.right_projection_map,
			                           ColumnBindingLayout(plan->children[1]->GetColumnBindings()), result.bindings);
		}
	}
	auto old_independent_bindings = plan->children[independent_idx]->GetColumnBindings();
	auto independent_state = DecorrelateIndependentSubtree(plan->children[independent_idx]);
	ColumnBindingRewrite::ApplyToChild(plan, independent_idx, std::move(old_independent_bindings),
	                                   independent_state.replacement_graph);
	result.replacement_graph.Merge(independent_state.replacement_graph);
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownCorrelatedNode(unique_ptr<LogicalOperator> &plan,
                                                                                    bool propagate_null_values) {
	auto state = PushDownCorrelatedNode(plan, propagate_null_values, {});
	if (!replacement_map.empty()) {
		RewriteCorrelatedAggregates::Rewrite(*plan, replacement_map);
	}
	if (!parent) {
		LogicalPlanVerifier::Verify(binder.context, *plan);
	}
	return state;
}

void FlattenDependentJoins::AppendCorrelatedColumns(vector<unique_ptr<Expression>> &expressions,
                                                    const vector<ColumnBinding> &state, bool include_names) const {
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto &col = correlated_columns[i];
		if (include_names) {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(col.name, col.type, state[i]));
		} else {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(col.type, state[i]));
		}
	}
}

void FlattenDependentJoins::AddDelimColumnsToGroup(LogicalAggregate &aggr, const vector<ColumnBinding> &state) const {
	auto key_count = perform_delim ? correlated_columns.size() : 1;
	for (idx_t i = 0; i < key_count; i++) {
		auto &col = correlated_columns[GetDelimKeyIndex(i)];
		auto colref = make_uniq<BoundColumnRefExpression>(col.name, col.type, state[GetDelimKeyIndex(i)]);
		auto new_group_index = ColumnBinding::PushExpression(aggr.groups, std::move(colref));
		for (auto &set : aggr.grouping_sets) {
			set.insert(new_group_index);
		}
	}
}

void FlattenDependentJoins::AddCorrelatedFirstAggregates(LogicalAggregate &aggr,
                                                         const vector<ColumnBinding> &state) const {
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto &col = correlated_columns[i];
		auto colref = make_uniq<BoundColumnRefExpression>(col.name, col.type, state[i]);
		vector<unique_ptr<Expression>> aggr_children;
		aggr_children.push_back(std::move(colref));

		auto first_aggregate = GetBuiltinAggregateFunction(binder.context, FirstFun::Name, {col.type});
		FunctionBinder function_binder(binder.context);
		auto first_fun = function_binder.BindAggregateFunction(std::move(first_aggregate), std::move(aggr_children),
		                                                       nullptr, AggregateType::NON_DISTINCT);
		aggr.expressions.push_back(std::move(first_fun));
	}
}

static bool
ExpressionCanThrowOnNullInput(ClientContext &context, const Expression &expr,
                              const std::function<bool(const BoundColumnRefExpression &)> &column_becomes_null) {
	bool can_throw = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (can_throw || !child.CanThrow()) {
			return;
		}
		// CanThrow is conservative for foldable children such as implicit constant casts.
		Value constant_value;
		if (child.IsConsistent() && child.IsFoldable() &&
		    ExpressionExecutor::TryEvaluateScalar(context, child, constant_value)) {
			return;
		}
		if (!ExpressionBecomesNull(child, column_becomes_null) ||
		    ExpressionCanThrowOnNullInput(context, child, column_becomes_null)) {
			// Independent children are evaluated before the parent can propagate NULL.
			can_throw = true;
		}
	});
	return can_throw;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownProjection(unique_ptr<LogicalOperator> &plan,
                                                                                bool propagate_null_values,
                                                                                vector<ColumnBinding> state) {
	if (propagate_null_values) {
		auto child_output = plan->children[0]->GetColumnBindings();
		column_binding_set_t child_bindings(child_output.begin(), child_output.end());
		auto column_becomes_null = [&](const BoundColumnRefExpression &column) {
			return column.Depth() == 0 && child_bindings.find(column.Binding()) != child_bindings.end();
		};
		for (auto &expr : plan->expressions) {
			if (expr->IsVolatile() || !ExpressionBecomesNull(*expr, column_becomes_null) ||
			    ExpressionCanThrowOnNullInput(binder.context, *expr, column_becomes_null)) {
				propagate_null_values = false;
				break;
			}
		}
	}

	propagate_null_values &= plan->children[0]->type != LogicalOperatorType::LOGICAL_DEPENDENT_JOIN;
	auto result = PushDownChild(plan, propagate_null_values, std::move(state));
	AppendCorrelatedColumns(plan->expressions, result.bindings, true);
	auto &proj = plan->Cast<LogicalProjection>();
	auto correlated_offset = plan->expressions.size() - correlated_columns.size();
	if (!parent) {
		LogicalPlanVerifier::Verify(binder.context, *plan);
	}
	result.bindings = CreateContiguousState(ColumnBinding(proj.table_index, ProjectionIndex(correlated_offset)));
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownAggregate(unique_ptr<LogicalOperator> &plan,
                                                                               bool propagate_null_values,
                                                                               vector<ColumnBinding> state) {
	auto &aggr = plan->Cast<LogicalAggregate>();
	for (auto &expr : plan->expressions) {
		auto &aggr_expr = expr->Cast<BoundAggregateExpression>();
		propagate_null_values &= !aggr_expr.IsVolatile() && aggr_expr.PropagatesNullValues();
	}
	// the aggregate observes child row presence through groups, grouping sets and GROUPING()
	auto result = PushDownChild(plan, false, std::move(state));

	TableIndex delim_table_index;
	idx_t delim_column_offset;
	auto new_group_count = perform_delim ? correlated_columns.size() : 1;
	AddDelimColumnsToGroup(aggr, result.bindings);
	if (!perform_delim) {
		delim_table_index = aggr.aggregate_index;
		delim_column_offset = aggr.expressions.size();
		AddCorrelatedFirstAggregates(aggr, result.bindings);
	} else {
		delim_table_index = aggr.group_index;
		delim_column_offset = aggr.groups.size() - correlated_columns.size();
	}
	// grouping sets that hold only the delim columns were originally empty - each one emits a row per delim key
	idx_t empty_set_count = 0;
	if (aggr.grouping_sets.empty()) {
		empty_set_count = aggr.groups.size() == new_group_count ? 1 : 0;
	} else {
		for (auto &grouping_set : aggr.grouping_sets) {
			if (grouping_set.size() == new_group_count) {
				empty_set_count++;
			}
		}
	}
	if (empty_set_count == 0) {
		result.bindings = CreateContiguousState(ColumnBinding(delim_table_index, ProjectionIndex(delim_column_offset)));
		return result;
	}

	// dropping missing keys is only equivalent when the single recovered row would be all-NULL:
	// repeated empty sets need one row per set and GROUPING() outputs are non-NULL constants
	auto drop_missing_keys =
	    !any_join && propagate_null_values && empty_set_count == 1 && aggr.grouping_functions.empty();
	auto join_type = drop_missing_keys ? JoinType::INNER : JoinType::LEFT;

	unique_ptr<LogicalComparisonJoin> join = make_uniq<LogicalComparisonJoin>(join_type);
	auto left_index = binder.GenerateTableIndex();
	auto delim_scan = make_uniq<LogicalDelimGet>(left_index, delim_types);
	join->children.push_back(std::move(delim_scan));
	join->children.push_back(std::move(plan));
	for (idx_t i = 0; i < new_group_count; i++) {
		auto &col = correlated_columns[GetDelimKeyIndex(i)];
		JoinCondition cond(
		    make_uniq<BoundColumnRefExpression>(col.name, col.type, ColumnBinding(left_index, ProjectionIndex(i))),
		    make_uniq<BoundColumnRefExpression>(
		        col.type, ColumnBinding(delim_table_index, ProjectionIndex(delim_column_offset + i))),
		    ExpressionType::COMPARE_NOT_DISTINCT_FROM);
		join->conditions.push_back(std::move(cond));
	}
	vector<ColumnBinding> special_handling_bindings;
	vector<unique_ptr<Expression>> empty_aggregates;
	vector<LogicalType> special_handling_types;
	for (idx_t i = 0, aggregate_count = aggr.expressions.size(); i < aggregate_count; i++) {
		D_ASSERT(aggr.expressions[i]->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
		auto &bound_func = aggr.expressions[i]->Cast<BoundAggregateExpression>().Function();
		if (bound_func.GetNullHandling() == FunctionNullHandling::SPECIAL_HANDLING) {
			special_handling_bindings.emplace_back(aggr.aggregate_index, ProjectionIndex(i));
			special_handling_types.push_back(aggr.expressions[i]->GetReturnType());
			empty_aggregates.push_back(aggr.expressions[i]->Copy());
		}
	}
	// recovered rows belong to an originally empty grouping set: substitute empty-input aggregate values and
	// GROUPING() masks, and emit one recovered row per empty grouping set
	auto grouping_count = aggr.grouping_functions.size();
	bool needs_empty_results = !empty_aggregates.empty() || grouping_count > 0;
	bool needs_multiplier = empty_set_count > 1;
	ColumnBinding marker_binding;
	if (needs_empty_results || needs_multiplier) {
		// recovered rows are detected by a NULL marker: count(*) is non-NULL for every aggregate row
		auto marker_index = ProjectionIndex(aggr.expressions.size());
		FunctionBinder function_binder(binder.context);
		aggr.expressions.push_back(
		    function_binder.BindAggregateFunction(GetBuiltinAggregateFunction(binder.context, CountStarFun::Name, {}),
		                                          {}, nullptr, AggregateType::NON_DISTINCT));
		marker_binding = ColumnBinding(aggr.aggregate_index, marker_index);
	}
	unique_ptr<LogicalOperator> empty_results;
	if (needs_empty_results) {
		auto special_count = empty_aggregates.size();
		unique_ptr<LogicalOperator> empty_child;
		TableIndex empty_aggregate_index;
		if (!empty_aggregates.empty()) {
			aggr.children[0]->ResolveOperatorTypes();
			auto child_bindings = aggr.children[0]->GetColumnBindings();
			auto empty_input_bindings =
			    LogicalOperator::GenerateColumnBindings(binder.GenerateTableIndex(), child_bindings.size());
			auto empty_input = make_uniq<LogicalEmptyResult>(aggr.children[0]->types, empty_input_bindings);
			empty_aggregate_index = binder.GenerateTableIndex();
			auto aggregate = make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), empty_aggregate_index,
			                                             std::move(empty_aggregates));
			ColumnBindingReplacer replacer;
			replacer.AddReplacements(child_bindings, empty_input_bindings);
			replacer.VisitOperatorBindings(*aggregate);
			aggregate->children.push_back(std::move(empty_input));
			empty_child = std::move(aggregate);
		} else {
			empty_child = make_uniq<LogicalDummyScan>(binder.GenerateTableIndex());
		}
		vector<ColumnBinding> empty_result_bindings;
		if (grouping_count == 0) {
			for (idx_t i = 0; i < special_count; i++) {
				empty_result_bindings.emplace_back(empty_aggregate_index, ProjectionIndex(i));
			}
			empty_results = std::move(empty_child);
		} else {
			// project the empty-input aggregate values plus the GROUPING() masks of an empty grouping set
			vector<unique_ptr<Expression>> empty_exprs;
			for (idx_t i = 0; i < special_count; i++) {
				empty_exprs.push_back(make_uniq<BoundColumnRefExpression>(
				    special_handling_types[i], ColumnBinding(empty_aggregate_index, ProjectionIndex(i))));
			}
			for (auto &grouping_function : aggr.grouping_functions) {
				int64_t grouping_value = 0;
				for (idx_t i = 0; i < grouping_function.size(); i++) {
					// no column of an empty grouping set is grouped
					grouping_value += 1LL << (grouping_function.size() - (i + 1));
				}
				empty_exprs.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(grouping_value)));
			}
			auto empty_projection_index = binder.GenerateTableIndex();
			auto empty_projection = make_uniq<LogicalProjection>(empty_projection_index, std::move(empty_exprs));
			empty_projection->children.push_back(std::move(empty_child));
			empty_results = std::move(empty_projection);
			for (idx_t i = 0; i < special_count + grouping_count; i++) {
				empty_result_bindings.emplace_back(empty_projection_index, ProjectionIndex(i));
			}
		}
		for (idx_t i = 0; i < special_count; i++) {
			replacement_map[special_handling_bindings[i]] = {marker_binding, empty_result_bindings[i]};
		}
		for (idx_t i = 0; i < grouping_count; i++) {
			replacement_map[ColumnBinding(aggr.groupings_index, ProjectionIndex(i))] = {
			    marker_binding, empty_result_bindings[special_count + i]};
		}
	}
	plan = std::move(join);
	if (empty_results) {
		// The RHS always produces one row, making this equivalent to a cross product.
		// Keep it non-reorderable because projection maps depend on the output order.
		auto cross_product = make_uniq<LogicalComparisonJoin>(JoinType::LEFT);
		cross_product->children.push_back(std::move(plan));
		cross_product->children.push_back(std::move(empty_results));
		plan = std::move(cross_product);
	}
	if (needs_multiplier) {
		// repeat recovered rows once per empty grouping set: the all-NULL rows match every multiplier row,
		// aggregate rows match none and are null-extended exactly once
		auto multiplier_index = binder.GenerateTableIndex();
		vector<LogicalType> multiplier_types {LogicalType::BIGINT};
		vector<vector<unique_ptr<Expression>>> multiplier_rows;
		for (idx_t i = 0; i < empty_set_count; i++) {
			vector<unique_ptr<Expression>> multiplier_row;
			multiplier_row.push_back(make_uniq<BoundConstantExpression>(Value(LogicalType::BIGINT)));
			multiplier_rows.push_back(std::move(multiplier_row));
		}
		auto multiplier =
		    make_uniq<LogicalExpressionGet>(multiplier_index, std::move(multiplier_types), std::move(multiplier_rows));
		multiplier->children.push_back(make_uniq<LogicalDummyScan>(binder.GenerateTableIndex()));
		auto multiplier_join = make_uniq<LogicalComparisonJoin>(JoinType::LEFT);
		JoinCondition multiplier_condition(
		    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, marker_binding),
		    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
		                                        ColumnBinding(multiplier_index, ProjectionIndex(0))),
		    ExpressionType::COMPARE_NOT_DISTINCT_FROM);
		multiplier_join->conditions.push_back(std::move(multiplier_condition));
		multiplier_join->children.push_back(std::move(plan));
		multiplier_join->children.push_back(std::move(multiplier));
		plan = std::move(multiplier_join);
	}
	result.bindings = CreateContiguousState(ColumnBinding(left_index, ProjectionIndex(0)));
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownCrossProduct(unique_ptr<LogicalOperator> &plan,
                                                                                  bool propagate_null_values,
                                                                                  vector<ColumnBinding> state) {
	if (!RequiresDomain(*plan->children[1])) {
		return PushDownSingleCorrelatedChild(plan, propagate_null_values, std::move(state), true);
	}
	if (!RequiresDomain(*plan->children[0])) {
		return PushDownSingleCorrelatedChild(plan, propagate_null_values, std::move(state), false);
	}

	auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	auto right_state = PushDownCorrelatedNode(plan->children[1], propagate_null_values, state);
	auto left_state = PushDownCorrelatedNode(plan->children[0], propagate_null_values, right_state.bindings);
	AddCorrelatedJoinConditions(*join, left_state.bindings, right_state.bindings);
	left_state.replacement_graph.Merge(right_state.replacement_graph);
	join->children.push_back(std::move(plan->children[0]));
	join->children.push_back(std::move(plan->children[1]));
	plan = std::move(join);
	return left_state;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownFullOuterJoin(unique_ptr<LogicalOperator> &plan,
                                                                                   bool propagate_null_values,
                                                                                   vector<ColumnBinding> state) {
	auto &join = plan->Cast<LogicalJoin>();
	auto left_payload =
	    ColumnBindingLayout(plan->children[0]->GetColumnBindings()).ProjectBindings(join.left_projection_map);
	auto right_payload =
	    ColumnBindingLayout(plan->children[1]->GetColumnBindings()).ProjectBindings(join.right_projection_map);

	auto left_state = PushDownChild(plan, propagate_null_values, std::move(state), false, 0);
	auto right_state = PushDownChild(plan, propagate_null_values, left_state.bindings, false, 1);

	AddCorrelatedJoinConditions(plan->Cast<LogicalJoin>(), left_state.bindings, right_state.bindings);
	RewriteCorrelatedBindings(*plan, right_state.bindings);

	for (auto &binding : left_payload) {
		binding = left_state.replacement_graph.Resolve(binding);
	}
	for (auto &binding : right_payload) {
		binding = right_state.replacement_graph.Resolve(binding);
	}
	auto left_output = ColumnBindingLayout(plan->children[0]->GetColumnBindings());
	auto right_output = ColumnBindingLayout(plan->children[1]->GetColumnBindings());
	auto &rewritten_join = plan->Cast<LogicalJoin>();
	rewritten_join.left_projection_map = left_output.CreateProjectionMap(left_payload);
	rewritten_join.right_projection_map = right_output.CreateProjectionMap(right_payload);
	AppendStateToProjectionMap(rewritten_join.left_projection_map, left_output, left_state.bindings);
	AppendStateToProjectionMap(rewritten_join.right_projection_map, right_output, right_state.bindings);
	plan->ResolveOperatorTypes();
	auto join_bindings = plan->GetColumnBindings();
	auto join_types = plan->types;
	D_ASSERT(join_bindings.size() == join_types.size());
	auto join_output = ColumnBindingLayout(join_bindings);
	auto payload_bindings = left_payload;
	payload_bindings.insert(payload_bindings.end(), right_payload.begin(), right_payload.end());

	vector<unique_ptr<Expression>> expressions;
	expressions.reserve(payload_bindings.size() + correlated_columns.size());
	for (auto &binding : payload_bindings) {
		auto position = join_output.GetPosition(binding);
		expressions.push_back(make_uniq<BoundColumnRefExpression>(join_types[position], binding));
	}
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto coalesce =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, correlated_columns[i].type);
		coalesce->GetChildrenMutable().push_back(
		    make_uniq<BoundColumnRefExpression>(correlated_columns[i].type, left_state.bindings[i]));
		coalesce->GetChildrenMutable().push_back(
		    make_uniq<BoundColumnRefExpression>(correlated_columns[i].type, right_state.bindings[i]));
		expressions.push_back(std::move(coalesce));
	}

	auto projection_index = binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	projection->children.push_back(std::move(plan));
	auto projection_bindings = projection->GetColumnBindings();
	left_state.replacement_graph.Merge(right_state.replacement_graph);
	for (idx_t i = 0; i < payload_bindings.size(); i++) {
		left_state.replacement_graph.Add(payload_bindings[i], projection_bindings[i]);
	}
	plan = std::move(projection);
	left_state.bindings =
	    CreateContiguousState(ColumnBinding(projection_index, ProjectionIndex(payload_bindings.size())));
	return left_state;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownJoin(unique_ptr<LogicalOperator> &plan,
                                                                          bool propagate_null_values,
                                                                          vector<ColumnBinding> state) {
	auto join_type = plan->Cast<LogicalJoin>().join_type;
	D_ASSERT(plan->children.size() == 2);
	bool left_requires_domain = RequiresDomain(*plan->children[0]);
	bool right_requires_domain = RequiresDomain(*plan->children[1]);

	bool can_push_left_only = !right_requires_domain && (join_type == JoinType::INNER || join_type == JoinType::LEFT);
	bool can_push_right_only = !left_requires_domain && (join_type == JoinType::INNER || join_type == JoinType::RIGHT);

	if (can_push_left_only) {
		return PushDownSingleCorrelatedChild(plan, propagate_null_values, std::move(state), true);
	}
	if (can_push_right_only) {
		return PushDownSingleCorrelatedChild(plan, propagate_null_values, std::move(state), false);
	}
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		if (!left_requires_domain && !right_requires_domain) {
			// The correlation exists only in the predicate. Attach the active delim
			// state to the output side and decorrelate the other side independently.
			auto old_right_bindings = plan->children[1]->GetColumnBindings();
			auto right_state = DecorrelateIndependentSubtree(plan->children[1]);
			ColumnBindingRewrite::ApplyToChild(plan, 1, std::move(old_right_bindings), right_state.replacement_graph);

			auto left_state = AttachDelimToIndependentJoinLeft(plan);
			RewriteCorrelatedBindings(*plan, left_state.bindings);
			left_state.replacement_graph.Merge(right_state.replacement_graph);
			return left_state;
		}
		if (left_requires_domain && !right_requires_domain) {
			return PushDownSingleCorrelatedChild(plan, propagate_null_values, std::move(state), true);
		}
		if (!left_requires_domain && right_requires_domain) {
			// The output side is independent, so pair it with the active delim
			// state before applying the SEMI/ANTI predicate.
			auto right_state = PushDownChild(plan, propagate_null_values, state, false, 1);
			auto left_state = AttachDelimToIndependentJoinLeft(plan);
			AddCorrelatedJoinConditions(plan->Cast<LogicalJoin>(), left_state.bindings, right_state.bindings);
			RewriteCorrelatedBindings(*plan, left_state.bindings);
			right_state.replacement_graph.Merge(left_state.replacement_graph);
			right_state.bindings = std::move(left_state.bindings);
			return right_state;
		}
		if (left_requires_domain && right_requires_domain) {
			// SEMI/ANTI only emit the left side. Decorrelate both inputs and join
			// on the state columns, but return the state carried by the output side.
			auto right_state = PushDownChild(plan, propagate_null_values, state, false, 1);
			auto left_state = PushDownChild(plan, propagate_null_values, right_state.bindings, false, 0);
			auto &rewritten_join = plan->Cast<LogicalJoin>();
			AppendStateToProjectionMap(rewritten_join.left_projection_map,
			                           ColumnBindingLayout(plan->children[0]->GetColumnBindings()),
			                           left_state.bindings);
			AddCorrelatedJoinConditions(rewritten_join, left_state.bindings, right_state.bindings);
			RewriteCorrelatedBindings(*plan, left_state.bindings);
			left_state.replacement_graph.Merge(right_state.replacement_graph);
			return left_state;
		}
	}

	if (join_type == JoinType::MARK) {
		if (right_requires_domain) {
			auto right_state = PushDownChild(plan, propagate_null_values, state, false, 1);

			auto left_state = PushDownChild(plan, propagate_null_values, right_state.bindings, false, 0);

			auto &comparison_join = plan->Cast<LogicalComparisonJoin>();
			auto conditions = std::move(comparison_join.conditions);
			comparison_join.conditions.clear();
			AddCorrelatedJoinConditions(comparison_join, left_state.bindings, right_state.bindings);
			// Keep the correlation keys before the IN predicate for per-group NULL and empty-set handling.
			comparison_join.mark_types.insert(comparison_join.mark_types.begin(), delim_types.begin(),
			                                  delim_types.end());
			for (auto &condition : conditions) {
				comparison_join.conditions.push_back(std::move(condition));
			}
			RewriteCorrelatedBindings(*plan, left_state.bindings);
			left_state.replacement_graph.Merge(right_state.replacement_graph);
			return left_state;
		}

		auto left_state = PushDownChild(plan, propagate_null_values, state, false);
		auto old_right_bindings = plan->children[1]->GetColumnBindings();
		auto independent_state = DecorrelateIndependentSubtree(plan->children[1]);
		ColumnBindingRewrite::ApplyToChild(plan, 1, std::move(old_right_bindings), independent_state.replacement_graph);
		left_state.replacement_graph.Merge(independent_state.replacement_graph);
		RewriteCorrelatedBindings(*plan, left_state.bindings);
		return left_state;
	}

	if (join_type == JoinType::OUTER) {
		return PushDownFullOuterJoin(plan, propagate_null_values, std::move(state));
	}

	if (join_type != JoinType::INNER && join_type != JoinType::LEFT && join_type != JoinType::RIGHT) {
		throw NotImplementedException("Unsupported join type for flattening correlated subquery");
	}

	auto left_state = PushDownChild(plan, propagate_null_values, state, false);

	auto right_state = PushDownChild(plan, propagate_null_values, left_state.bindings, false, 1);

	AddCorrelatedJoinConditions(plan->Cast<LogicalJoin>(), left_state.bindings, right_state.bindings);
	RewriteCorrelatedBindings(*plan, right_state.bindings);
	left_state.replacement_graph.Merge(right_state.replacement_graph);

	auto &rewritten_join = plan->Cast<LogicalJoin>();
	if (rewritten_join.join_type == JoinType::LEFT) {
		AppendStateToProjectionMap(rewritten_join.left_projection_map,
		                           ColumnBindingLayout(plan->children[0]->GetColumnBindings()), left_state.bindings);
		return left_state;
	}
	AppendStateToProjectionMap(rewritten_join.right_projection_map,
	                           ColumnBindingLayout(plan->children[1]->GetColumnBindings()), right_state.bindings);
	left_state.bindings = right_state.bindings;
	return left_state;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownLimit(unique_ptr<LogicalOperator> &plan,
                                                                           bool propagate_null_values,
                                                                           vector<ColumnBinding> state) {
	auto &limit = plan->Cast<LogicalLimit>();
	switch (limit.limit_val.Type()) {
	case LimitNodeType::CONSTANT_PERCENTAGE:
	case LimitNodeType::EXPRESSION_PERCENTAGE:
		throw ParserException("Limit percent operator not supported in correlated subquery");
	case LimitNodeType::EXPRESSION_VALUE:
		throw ParserException("Non-constant limit not supported in correlated subquery");
	default:
		break;
	}
	switch (limit.offset_val.Type()) {
	case LimitNodeType::EXPRESSION_VALUE:
		throw ParserException("Non-constant offset not supported in correlated subquery");
	case LimitNodeType::CONSTANT_PERCENTAGE:
	case LimitNodeType::EXPRESSION_PERCENTAGE:
		throw InternalException("Percentage offset in FlattenDependentJoin");
	default:
		break;
	}
	unique_ptr<LogicalOrder> order_by = nullptr;
	if (plan->children[0]->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		order_by = unique_ptr_cast<LogicalOperator, LogicalOrder>(std::move(plan->children[0]));
		plan->children[0] = std::move(order_by->children[0]); // Skip the order_by in the tree temporarily
	}

	auto result = PushDownCorrelatedNode(plan->children[0], propagate_null_values, std::move(state));
	if (order_by) {
		ColumnBindingRewrite::ApplyToOperatorBindings(*order_by, result.replacement_graph);
	}
	auto child = std::move(plan->children[0]);
	auto child_column_count = child->GetColumnBindings().size();

	auto window_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> partitions;
	auto delim_key_count = perform_delim ? correlated_columns.size() : 1;
	for (idx_t i = 0; i < delim_key_count; i++) {
		auto &col = correlated_columns[GetDelimKeyIndex(i)];
		partitions.push_back(
		    make_uniq<BoundColumnRefExpression>(col.name, col.type, result.bindings[GetDelimKeyIndex(i)]));
	}

	auto window = CreateRowNumberWindow(binder, std::move(child), window_index, std::move(partitions),
	                                    order_by ? std::move(order_by->orders) : vector<BoundOrderByNode>());

	auto filter = make_uniq<LogicalFilter>();
	auto row_num_ref = make_uniq<BoundColumnRefExpression>("limit_rownum", LogicalType::BIGINT,
	                                                       ColumnBinding(window_index, ProjectionIndex(0)));
	unique_ptr<Expression> condition = nullptr;

	if (limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		auto limit_val = limit.limit_val.GetConstantValue();
		if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE &&
		    !TryAddOperator::Operation(limit_val, limit.offset_val.GetConstantValue(), limit_val)) {
			limit_val = NumericLimits<idx_t>::Maximum();
		}
		limit_val = MinValue<idx_t>(limit_val, idx_t(NumericLimits<int64_t>::Maximum()));
		auto upper_bound = make_uniq<BoundConstantExpression>(int64_t(limit_val));
		condition = BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHANOREQUALTO, row_num_ref->Copy(),
		                                              std::move(upper_bound));
	}

	if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		auto lower_bound = make_uniq<BoundConstantExpression>(int64_t(limit.offset_val.GetConstantValue()));
		auto lower_comp = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, row_num_ref->Copy(),
		                                                    std::move(lower_bound));

		// Stitch together with AND if both bounds exist
		condition = condition ? make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
		                                                              std::move(lower_comp), std::move(condition))
		                      : std::move(lower_comp);
	}

	filter->expressions.push_back(std::move(condition));
	filter->children.push_back(std::move(window));
	for (idx_t i = 0; i < child_column_count; i++) {
		filter->projection_map.emplace_back(i);
	}

	plan = std::move(filter);
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownWindow(unique_ptr<LogicalOperator> &plan,
                                                                            bool propagate_null_values,
                                                                            vector<ColumnBinding> state) {
	auto &window = plan->Cast<LogicalWindow>();
	// window functions such as row_number() observe child row presence
	auto result = PushDownChild(plan, false, std::move(state));

	for (auto &expr : window.expressions) {
		D_ASSERT(expr->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
		auto &w = expr->Cast<BoundWindowExpression>();
		AppendCorrelatedColumns(w.PartitionsMutable(), result.bindings, false);
	}
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownSetOperation(unique_ptr<LogicalOperator> &plan,
                                                                                  vector<ColumnBinding> state) {
	auto &setop = plan->Cast<LogicalSetOperation>();
	UnnestingState result(std::move(state));
#ifdef DEBUG
	for (auto &child : plan->children) {
		child->ResolveOperatorTypes();
	}
	for (idx_t i = 1; i < plan->children.size(); i++) {
		D_ASSERT(plan->children[0]->types.size() == plan->children[i]->types.size());
	}
#endif
	for (auto &child : plan->children) {
		// a dropped all-NULL branch row changes the cardinality observable alongside the other branches
		auto child_state = PushDownCorrelatedNode(child, false, std::move(result.bindings));
		result.bindings = child_state.bindings;
		result.replacement_graph.Merge(child_state.replacement_graph);
	}
	for (idx_t i = 0; i < plan->children.size(); i++) {
		if (plan->children[i]->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			plan->children[i] =
			    LogicalProjection::CreateIdentity(binder.GenerateTableIndex(), std::move(plan->children[i]));
		}
	}

#ifdef DEBUG
	for (idx_t i = 1; i < plan->children.size(); i++) {
		D_ASSERT(plan->children[0]->GetColumnBindings().size() == plan->children[i]->GetColumnBindings().size());
	}
	for (auto &child : plan->children) {
		child->ResolveOperatorTypes();
	}
	for (idx_t i = 1; i < plan->children.size(); i++) {
		D_ASSERT(plan->children[0]->types.size() == plan->children[i]->types.size());
	}
#endif
	result.bindings = CreateContiguousState(ColumnBinding(setop.table_index, ProjectionIndex(setop.column_count)));
	setop.column_count += correlated_columns.size();
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownDistinct(unique_ptr<LogicalOperator> &plan,
                                                                              bool propagate_null_values,
                                                                              vector<ColumnBinding> state) {
	auto result = PushDownChild(plan, propagate_null_values, std::move(state), false);
	auto &distinct = plan->Cast<LogicalDistinct>();
	AppendCorrelatedColumns(distinct.distinct_targets, result.bindings, false);
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownExpressionGet(unique_ptr<LogicalOperator> &plan,
                                                                                   bool propagate_null_values,
                                                                                   vector<ColumnBinding> state) {
	auto result = PushDownChild(plan, propagate_null_values, std::move(state));

	// Rewrite any depth>0 correlated refs already in the expression lists (VALUES (NEW.col))
	// before appending the delim-get bindings for the join condition.
	RewriteCorrelatedBindings(*plan, result.bindings);

	auto &expr_get = plan->Cast<LogicalExpressionGet>();
	for (auto &expr_list : expr_get.expressions) {
		AppendCorrelatedColumns(expr_list, result.bindings, false);
	}
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		expr_get.expr_types.push_back(correlated_columns[i].type);
	}
	auto correlated_offset = expr_get.expr_types.size() - correlated_columns.size();
	result.bindings = CreateContiguousState(ColumnBinding(expr_get.table_index, ProjectionIndex(correlated_offset)));
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownDML(unique_ptr<LogicalOperator> &plan,
                                                                         bool propagate_null_values,
                                                                         vector<ColumnBinding> state) {
	auto result = PushDownChild(plan, propagate_null_values, std::move(state));
	if (plan->type == LogicalOperatorType::LOGICAL_INSERT || plan->type == LogicalOperatorType::LOGICAL_UPDATE) {
		// PushDownChild appended the correlated columns to the child projection.
		// PhysicalInsert requires an exact column count and PhysicalUpdate requires the row-id last,
		// so remove the appended columns. The DELIM_GET below re-supplies them.
		// DELETE is skipped because PhysicalDelete reads the row-id by a fixed index.
		auto &child = *plan->children[0];
		if (child.type == LogicalOperatorType::LOGICAL_PROJECTION &&
		    child.expressions.size() > correlated_columns.size()) {
			child.expressions.resize(child.expressions.size() - correlated_columns.size());
			child.ResolveOperatorTypes();
		}
	}
	// DML output does not carry the child columns, so re-expose the correlation keys in a separate DELIM_GET that
	// the parent DelimJoin can reference.
	auto expose_idx = binder.GenerateTableIndex();
	unique_ptr<LogicalOperator> expose_delim = make_uniq<LogicalDelimGet>(expose_idx, delim_types);
	plan = LogicalCrossProduct::Create(std::move(plan), std::move(expose_delim));
	result.bindings = CreateContiguousState(ColumnBinding(expose_idx, ProjectionIndex(0)));
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownGet(unique_ptr<LogicalOperator> &plan,
                                                                         vector<ColumnBinding> state) {
	auto &get = plan->Cast<LogicalGet>();
	if (get.children.size() != 1) {
		throw InternalException("Flatten dependent joins - logical get encountered without children");
	}
	auto old_child_bindings = plan->children[0]->GetColumnBindings();
	auto result = PushDownCorrelatedNode(plan->children[0], true, std::move(state));
	ColumnBindingRewrite::ApplyToChild(plan, 0, std::move(old_child_bindings), result.replacement_graph);
	// find position of each correlated binding in the child's output
	auto child_bindings = plan->children[0]->GetColumnBindings();
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto it = std::find(child_bindings.begin(), child_bindings.end(), result.bindings[i]);
		if (it == child_bindings.end()) {
			throw InternalException("Flatten dependent joins - correlated binding not found in child output");
		}
		get.projected_input.push_back(NumericCast<idx_t>(it - child_bindings.begin()));
	}

	RewriteCorrelatedBindings(*plan, result.bindings);
	// state bindings pass through LogicalGet's projected_input, no update needed
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownCTE(unique_ptr<LogicalOperator> &plan,
                                                                         bool propagate_null_values,
                                                                         vector<ColumnBinding> state) {
#ifdef DEBUG
	plan->children[0]->ResolveOperatorTypes();
	plan->children[1]->ResolveOperatorTypes();
#endif
	if (plan->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && !RequiresDomain(*plan->children[0])) {
		UnnestingState result(std::move(state));
		if (RequiresDomain(*plan->children[1])) {
			auto old_right_bindings = plan->children[1]->GetColumnBindings();
			result = PushDownCorrelatedNode(plan->children[1], propagate_null_values, std::move(result.bindings));
			ColumnBindingRewrite::ApplyToChild(plan, 1, std::move(old_right_bindings), result.replacement_graph);
			DecorrelateIndependentSubtree(plan->children[0]);
		}
		return result;
	}

	auto old_left_bindings = plan->children[0]->GetColumnBindings();
	// the CTE definition is scanned in contexts this chain does not see
	auto result = PushDownCorrelatedNode(plan->children[0], false, std::move(state));
	ColumnBindingRewrite::ApplyToChild(plan, 0, std::move(old_left_bindings), result.replacement_graph);

	auto &cte = plan->Cast<LogicalCTE>();

	cte.correlated_columns = correlated_columns;
	binder.recursive_ctes[cte.table_index] = &cte;
	PatchAccessingOperators(*plan->children[1], cte.table_index, correlated_columns);
	InvalidateAccessCache();

	// The CTE outputs a contiguous block of bindings. Lock this state in.
	auto cte_state = CreateContiguousState(ColumnBinding(cte.table_index, ProjectionIndex(cte.column_count)));

	// Recursive CTE Specific Setup
	if (plan->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		auto &rec_cte = plan->Cast<LogicalRecursiveCTE>();
		if (!rec_cte.key_targets.empty()) {
			AppendCorrelatedColumns(rec_cte.key_targets, cte_state, false);
		}
		for (auto &col : correlated_columns) {
			rec_cte.internal_types.push_back(col.type);
		}
	}

	auto old_right_bindings = plan->children[1]->GetColumnBindings();
	auto right_state = PushDownCorrelatedNode(plan->children[1], false, cte_state);
	ColumnBindingRewrite::ApplyToChild(plan, 1, std::move(old_right_bindings), right_state.replacement_graph);
	RewriteCorrelatedBindings(*plan, right_state.bindings);
	result.replacement_graph.Merge(right_state.replacement_graph);

#ifdef DEBUG
	plan->children[0]->ResolveOperatorTypes();
	plan->children[1]->ResolveOperatorTypes();
#endif

	cte.column_count += correlated_columns.size();

	result.bindings =
	    (plan->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) ? std::move(cte_state) : right_state.bindings;
	return result;
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownCTERef(unique_ptr<LogicalOperator> &plan) {
	auto &cteref = plan->Cast<LogicalCTERef>();
	auto positions = GetCTERefCorrelatedPositions(cteref);
	vector<ColumnBinding> cte_state;
	cte_state.reserve(positions.size());
	for (auto &position : positions) {
		if (!position.IsValid()) {
			break;
		}
		cte_state.emplace_back(cteref.table_index, ProjectionIndex(position.GetIndex()));
	}
	if (cte_state.size() == correlated_columns.size()) {
		return UnnestingState(std::move(cte_state));
	}

	// Missing correlations require a domain scan; constrain it by every correlation shared with the CTE.
	auto delim_index = binder.GenerateTableIndex();
	auto delim_state = CreateContiguousState(ColumnBinding(delim_index, ProjectionIndex(0)));
	auto delim_scan = make_uniq<LogicalDelimGet>(delim_index, delim_types);
	auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	AddCTERefJoinConditions(*join, cteref, positions, delim_state);
	if (!join->conditions.empty()) {
		join->children.push_back(std::move(plan));
		join->children.push_back(std::move(delim_scan));
		plan = std::move(join);
		return UnnestingState(std::move(delim_state));
	}
	return UnnestingState(CreateDelimCrossProduct(plan, std::move(delim_scan), std::move(delim_state)));
}

FlattenDependentJoins::UnnestingState FlattenDependentJoins::PushDownCorrelatedNode(unique_ptr<LogicalOperator> &plan,
                                                                                    bool propagate_null_values,
                                                                                    vector<ColumnBinding> state) {
	auto result = PushDownCorrelatedNodeInternal(plan, propagate_null_values, std::move(state));
	InvalidateAccessCache();
	D_ASSERT(VerifyUnnestingState(*plan, result));
	return result;
}

bool FlattenDependentJoins::VerifyUnnestingState(LogicalOperator &plan, const UnnestingState &state) const {
	if (state.bindings.size() != correlated_columns.size()) {
		return false;
	}
	auto output = ColumnBindingLayout(plan.GetColumnBindings(), "decorrelated subtree output");
	column_binding_set_t active_bindings;
	for (auto &binding : state.bindings) {
		if (!active_bindings.insert(binding).second || output.positions.find(binding) == output.positions.end()) {
			return false;
		}
	}
	column_binding_set_t replaced_bindings;
	for (auto &replacement : state.replacement_graph) {
		if (replacement.old_binding == replacement.new_binding ||
		    !replaced_bindings.insert(replacement.old_binding).second) {
			return false;
		}
		auto resolved_old = state.replacement_graph.Resolve(replacement.old_binding);
		auto resolved_new = state.replacement_graph.Resolve(replacement.new_binding);
		if (resolved_old == replacement.old_binding || resolved_old != resolved_new) {
			return false;
		}
	}
	return true;
}

FlattenDependentJoins::UnnestingState
FlattenDependentJoins::PushDownCorrelatedNodeInternal(unique_ptr<LogicalOperator> &plan, bool propagate_null_values,
                                                      vector<ColumnBinding> state) {
	if (!RequiresDomain(*plan)) {
		if (plan->type == LogicalOperatorType::LOGICAL_CTE_REF) {
			return PushDownCTERef(plan);
		}
		return AttachDomainToIndependentSubtree(plan, propagate_null_values);
	}
	switch (plan->type) {
	case LogicalOperatorType::LOGICAL_FILTER: {
		for (auto &expr : plan->expressions) {
			if (expr->IsVolatile() || expr->CanThrow()) {
				propagate_null_values = false;
				break;
			}
		}
		return PushDownChild(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_UNNEST: {
		return PushDownChild(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		return PushDownProjection(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		return PushDownAggregate(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		// a dropped all-NULL row multiplies with the other side's rows, which are not all-NULL
		return PushDownCrossProduct(plan, false, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN: {
		D_ASSERT(plan->children.size() == 2);
		return DecorrelateSubtree(plan, false, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		return PushDownJoin(plan, false, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_LIMIT: {
		return PushDownLimit(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_WINDOW: {
		return PushDownWindow(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_UNION: {
		return PushDownSetOperation(plan, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		return PushDownDistinct(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
		// emits its expression rows once per child row
		return PushDownExpressionGet(plan, false, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE: {
		// the modified row count is a side effect
		return PushDownDML(plan, false, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_PIVOT:
		throw BinderException("PIVOT is not supported in correlated subqueries yet");
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		return PushDownChild(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_GET: {
		return PushDownGet(plan, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		return PushDownCTE(plan, propagate_null_values, std::move(state));
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		return PushDownCTERef(plan);
	}
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		throw BinderException("Nested lateral joins or lateral joins in correlated subqueries are not (yet) supported");
	}
	case LogicalOperatorType::LOGICAL_SAMPLE:
		throw BinderException("Sampling in correlated subqueries is not (yet) supported");
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
		throw BinderException("Positional join in correlated subqueries is not (yet) supported");
	default:
		throw InternalException("Logical operator type \"%s\" for dependent join", LogicalOperatorToString(plan->type));
	}
}

FlattenDependentJoins::UnnestingState
FlattenDependentJoins::AttachDomainToIndependentSubtree(unique_ptr<LogicalOperator> &plan, bool propagate_null_values) {
	// The delim state is not part of this subtree yet. Decorrelate nested dependent joins independently,
	// then attach it above the completed subtree.
	auto delim_index = binder.GenerateTableIndex();
	auto delim_state = CreateContiguousState(ColumnBinding(delim_index, ProjectionIndex(0)));
	auto delim_scan = make_uniq<LogicalDelimGet>(delim_index, delim_types);
	auto result = DecorrelateIndependentSubtree(plan, propagate_null_values);
	result.bindings = CreateDelimCrossProduct(plan, std::move(delim_scan), std::move(delim_state));
	return result;
}

} // namespace duckdb
