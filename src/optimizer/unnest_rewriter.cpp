#include "duckdb/optimizer/unnest_rewriter.hpp"

#include "duckdb/planner/expression/bound_cast_expression.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/planner/expression_binder.hpp"

#include <algorithm>

namespace duckdb {

static bool IsSupportedUnnestTop(LogicalOperatorType type) {
	return type == LogicalOperatorType::LOGICAL_PROJECTION || type == LogicalOperatorType::LOGICAL_WINDOW ||
	       type == LogicalOperatorType::LOGICAL_FILTER || type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY ||
	       type == LogicalOperatorType::LOGICAL_UNNEST;
}

static optional_idx FindBindingIndex(const vector<ColumnBinding> &bindings, const ColumnBinding &binding) {
	auto entry = std::find(bindings.begin(), bindings.end(), binding);
	if (entry == bindings.end()) {
		return optional_idx();
	}
	return NumericCast<idx_t>(entry - bindings.begin());
}

static idx_t CountUniqueBindings(const vector<ColumnBinding> &bindings) {
	vector<ColumnBinding> unique_bindings;
	for (auto &binding : bindings) {
		if (!FindBindingIndex(unique_bindings, binding).IsValid()) {
			unique_bindings.push_back(binding);
		}
	}
	return unique_bindings.size();
}

static idx_t CountCTERefs(LogicalOperator &op, TableIndex cte_index) {
	idx_t result = 0;
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF && op.Cast<LogicalCTERef>().cte_index == cte_index) {
		result++;
	}
	for (auto &child : op.children) {
		result += CountCTERefs(*child, cte_index);
	}
	return result;
}

static bool IsCTERef(LogicalOperator &op, TableIndex cte_index) {
	return op.type == LogicalOperatorType::LOGICAL_CTE_REF && op.Cast<LogicalCTERef>().cte_index == cte_index;
}

static optional_idx FindCTERefSide(LogicalComparisonJoin &join, TableIndex cte_index) {
	if (IsCTERef(*join.children[0], cte_index)) {
		return optional_idx(0);
	}
	if (IsCTERef(*join.children[1], cte_index)) {
		return optional_idx(1);
	}
	return optional_idx();
}

void UnnestRewriterPlanUpdater::VisitOperator(LogicalOperator &op) {
	VisitOperatorChildren(op);
	VisitOperatorExpressions(op);
}

void UnnestRewriterPlanUpdater::VisitExpression(unique_ptr<Expression> *expression) {
	auto &expr = *expression;

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &bound_column_ref = expr->Cast<BoundColumnRefExpression>();
		for (idx_t i = 0; i < replace_bindings.size(); i++) {
			if (bound_column_ref.Binding() == replace_bindings[i].old_binding) {
				bound_column_ref.BindingMutable() = replace_bindings[i].new_binding;
				break;
			}
		}
	}

	VisitExpressionChildren(**expression);
}

unique_ptr<LogicalOperator> UnnestRewriter::Optimize(unique_ptr<LogicalOperator> op) {
	UnnestRewriterPlanUpdater updater;
	vector<reference<unique_ptr<LogicalOperator>>> candidates;
	FindCandidates(op, op, candidates);

	// rewrite the plan and update the bindings
	for (auto &candidate : candidates) {
		// rearrange the logical operators
		if (RewriteCandidate(candidate)) {
			updater.overwritten_tbl_idx = overwritten_tbl_idx;
			// update the bindings of the BOUND_UNNEST expression
			UpdateBoundUnnestBindings(updater, candidate);
			// update the sequence of LOGICAL_PROJECTION(s)
			UpdateRHSBindings(op, candidate, updater);
			// reset
			delim_columns.clear();
			lhs_bindings.clear();
		}
	}

	RewriteCTECandidates(op, op, updater);
	return op;
}

void UnnestRewriter::FindCandidates(unique_ptr<LogicalOperator> &root, unique_ptr<LogicalOperator> &op,
                                    vector<reference<unique_ptr<LogicalOperator>>> &candidates) {
	// search children before adding, so that we add candidates bottom-up
	for (auto &child : op->children) {
		FindCandidates(root, child, candidates);
	}

	// search for operator that has a LOGICAL_DELIM_JOIN as its child
	if (op->children.size() != 1) {
		return;
	}
	if (op->children[0]->type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return;
	}

	// found a delim join
	auto &delim_join = op->children[0]->Cast<LogicalComparisonJoin>();
	// only support INNER delim joins
	if (delim_join.join_type != JoinType::INNER) {
		return;
	}
	// INNER delim join must have exactly one condition
	if (delim_join.conditions.size() != 1) {
		return;
	}

	// LHS child is a window
	idx_t delim_idx = delim_join.delim_flipped ? 1 : 0;
	idx_t other_idx = 1 - delim_idx;
	if (delim_join.children[delim_idx]->type != LogicalOperatorType::LOGICAL_WINDOW) {
		return;
	}

	// RHS child must be projection(s) followed by an UNNEST
	auto curr_op = &delim_join.children[other_idx];
	while (curr_op->get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		if (curr_op->get()->children.size() != 1) {
			break;
		}
		curr_op = &curr_op->get()->children[0];
	}

	// pattern1: delim_get -> unnest-> projection
	if (curr_op->get()->type == LogicalOperatorType::LOGICAL_UNNEST &&
	    curr_op->get()->children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		candidates.push_back(op);
		return;
	}

	curr_op = &delim_join.children[other_idx];
	if (curr_op->get()->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = curr_op->get()->Cast<LogicalGet>();
		if (!ExpressionBinder::IsUnnestFunction(get.function.GetName())) {
			return;
		}
		// pattern2: delim_get -> projection -> table_in_out(unnest)
		auto &unnest_get_ref = curr_op->get()->Cast<LogicalGet>();
		if (unnest_get_ref.ordinality_idx.IsValid()) {
			// we also unnest delim_index so cannot rewrite it
			return;
		}
		curr_op = &curr_op->get()->children[0];

		// find pattern2 and convert to pattern1
		if (curr_op->get()->type == LogicalOperatorType::LOGICAL_PROJECTION &&
		    curr_op->get()->children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
			// verify we can do the unnest rewrite optimization prior to making any changes
			auto &unnest_get_child = *delim_join.children[other_idx];
			auto unnest_get_column = unnest_get_child.GetColumnBindings();
			auto unnest_get_index = unnest_get_child.GetTableIndex()[0];
			unnest_get_child.ResolveOperatorTypes();

			auto &proj = curr_op->get()->Cast<LogicalProjection>();
			for (idx_t i = 0; i < unnest_get_column.size(); i++) {
				auto &col_bind = unnest_get_column[i];
				if (col_bind.table_index != unnest_get_index) {
					// not part of the unnest
					continue;
				}
				auto &expr = proj.GetExpression(col_bind.column_index);
				if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
					// unnest reference is not a projection reference - bail
					return;
				}
			}
			// perform the actual rewrite
			auto unnest_get = std::move(delim_join.children[other_idx]);
			ColumnBindingReplacer replacer;
			auto delim_get = std::move(proj.children[0]);
			auto unnest = make_uniq<LogicalUnnest>(unnest_get_index);
			unnest->children.push_back(std::move(delim_get));
			delim_join.children[other_idx] = std::move(*curr_op);
			for (idx_t i = 0; i < unnest_get_column.size(); i++) {
				auto &col_bind = unnest_get_column[i];
				D_ASSERT(col_bind.table_index == unnest_get_index || col_bind.table_index == proj.table_index);
				if (col_bind.table_index != unnest_get_index) {
					// not part of the unnest
					continue;
				}
				auto &bind_col = proj.expressions[col_bind.column_index]->Cast<BoundColumnRefExpression>();
				auto unnest_expr = make_uniq<BoundUnnestExpression>(unnest_get->types[i]);
				unnest_expr->ChildMutable() =
				    BoundCastExpression::AddArrayCastToList(context, proj.expressions[col_bind.column_index]->Copy());
				bind_col.BindingMutable() = ColumnBinding(unnest_get_index, bind_col.Binding().column_index);
				auto unnest_proj_idx = ColumnBinding::PushExpression(unnest->expressions, std::move(unnest_expr));
				ColumnBinding new_column_ref(bind_col.Binding().table_index, unnest_proj_idx);
				auto unnest_ref = make_uniq<BoundColumnRefExpression>(bind_col.GetAlias(), unnest_get->types[i],
				                                                      new_column_ref, bind_col.Depth());
				proj.expressions[col_bind.column_index] = std::move(unnest_ref);
				proj.types[col_bind.column_index] = unnest_get->types[i];
				replacer.replacement_bindings.push_back(ReplacementBinding(
				    col_bind, ColumnBinding(proj.table_index, col_bind.column_index), unnest_get->types[i]));
			}
			proj.children[0] = std::move(unnest);
			replacer.stop_operator = proj;
			replacer.VisitOperator(*root);
			candidates.push_back(op);
		}
	}
}

static bool ConvertCTETableInOutUnnest(ClientContext &context, unique_ptr<LogicalOperator> &root,
                                       unique_ptr<LogicalOperator> &op, TableIndex input_cte_index,
                                       bool require_input_cte_ref = true) {
	if (op->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = op->Cast<LogicalGet>();
	if (!ExpressionBinder::IsUnnestFunction(get.function.GetName()) || get.ordinality_idx.IsValid()) {
		return false;
	}
	if (op->children.size() != 1 || op->children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	auto &proj = op->children[0]->Cast<LogicalProjection>();
	if (proj.children.size() != 1) {
		return false;
	}
	if (require_input_cte_ref && !IsCTERef(*proj.children[0], input_cte_index)) {
		return false;
	}

	auto unnest_get_column = op->GetColumnBindings();
	auto unnest_get_index = op->GetTableIndex()[0];
	op->ResolveOperatorTypes();
	for (idx_t i = 0; i < unnest_get_column.size(); i++) {
		auto &col_bind = unnest_get_column[i];
		if (col_bind.table_index != unnest_get_index) {
			continue;
		}
		auto &expr = proj.GetExpression(col_bind.column_index);
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
	}

	auto unnest_get = std::move(op);
	ColumnBindingReplacer replacer;
	auto cte_ref = std::move(proj.children[0]);
	auto unnest = make_uniq<LogicalUnnest>(unnest_get_index);
	unnest->children.push_back(std::move(cte_ref));
	op = std::move(unnest_get->children[0]);
	for (idx_t i = 0; i < unnest_get_column.size(); i++) {
		auto &col_bind = unnest_get_column[i];
		D_ASSERT(col_bind.table_index == unnest_get_index || col_bind.table_index == proj.table_index);
		if (col_bind.table_index != unnest_get_index) {
			continue;
		}
		auto &bind_col = proj.expressions[col_bind.column_index]->Cast<BoundColumnRefExpression>();
		auto unnest_expr = make_uniq<BoundUnnestExpression>(unnest_get->types[i]);
		unnest_expr->ChildMutable() =
		    BoundCastExpression::AddArrayCastToList(context, proj.expressions[col_bind.column_index]->Copy());
		bind_col.BindingMutable() = ColumnBinding(unnest_get_index, bind_col.Binding().column_index);
		auto unnest_proj_idx = ColumnBinding::PushExpression(unnest->expressions, std::move(unnest_expr));
		ColumnBinding new_column_ref(bind_col.Binding().table_index, unnest_proj_idx);
		auto unnest_ref = make_uniq<BoundColumnRefExpression>(bind_col.GetAlias(), unnest_get->types[i], new_column_ref,
		                                                      bind_col.Depth());
		proj.expressions[col_bind.column_index] = std::move(unnest_ref);
		proj.types[col_bind.column_index] = unnest_get->types[i];
		replacer.replacement_bindings.push_back(
		    ReplacementBinding(col_bind, ColumnBinding(proj.table_index, col_bind.column_index), unnest_get->types[i]));
	}
	proj.children[0] = std::move(unnest);
	replacer.stop_operator = proj;
	replacer.VisitOperator(*root);
	return true;
}

static bool GetInlineDedupColumns(LogicalMaterializedCTE &domain_cte, LogicalOperator &dedup_op,
                                  vector<ColumnBinding> &dedup_columns) {
	// An earlier CTE inlining pass can remove the explicit dedup CTE. Trace the bindings emitted below UNNEST back
	// through projections and the aggregate groups to recover the original delimiter columns from the domain producer.
	domain_cte.children[0]->ResolveOperatorTypes();
	auto source_bindings = domain_cte.children[0]->GetColumnBindings();
	vector<ColumnBinding> traced_bindings = dedup_op.GetColumnBindings();

	reference<LogicalOperator> current_op(dedup_op);
	while (current_op.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = current_op.get().Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return false;
		}
		auto current_bindings = projection.GetColumnBindings();
		for (auto &binding : traced_bindings) {
			auto binding_idx = FindBindingIndex(current_bindings, binding);
			if (!binding_idx.IsValid()) {
				continue;
			}
			if (binding_idx.GetIndex() >= projection.expressions.size()) {
				return false;
			}
			auto &expr = projection.GetExpression(ProjectionIndex(binding_idx.GetIndex()));
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return false;
			}
			auto &colref = expr.Cast<BoundColumnRefExpression>();
			if (colref.Depth() != 0) {
				return false;
			}
			binding = colref.Binding();
		}
		current_op = *projection.children[0];
	}

	if (current_op.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = current_op.get().Cast<LogicalAggregate>();
	if (aggregate.children.size() != 1 || !IsCTERef(*aggregate.children[0], domain_cte.table_index)) {
		return false;
	}
	auto aggregate_bindings = aggregate.GetColumnBindings();
	auto aggregate_child_bindings = aggregate.children[0]->GetColumnBindings();
	for (auto &binding : traced_bindings) {
		auto aggregate_idx = FindBindingIndex(aggregate_bindings, binding);
		optional_idx source_idx;
		if (!aggregate_idx.IsValid()) {
			source_idx = FindBindingIndex(aggregate_child_bindings, binding);
		} else {
			if (aggregate_idx.GetIndex() >= aggregate.groups.size()) {
				return false;
			}
			auto &group = aggregate.groups[aggregate_idx.GetIndex()];
			if (group->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return false;
			}
			auto &colref = group->Cast<BoundColumnRefExpression>();
			if (colref.Depth() != 0) {
				return false;
			}
			source_idx = FindBindingIndex(aggregate_child_bindings, colref.Binding());
		}
		if (!source_idx.IsValid() || source_idx.GetIndex() >= source_bindings.size()) {
			return false;
		}
		dedup_columns.push_back(source_bindings[source_idx.GetIndex()]);
	}
	return !dedup_columns.empty();
}

bool UnnestRewriter::RewriteCTECandidates(unique_ptr<LogicalOperator> &root, unique_ptr<LogicalOperator> &op,
                                          UnnestRewriterPlanUpdater &updater) {
	bool changed = false;
	for (auto &child : op->children) {
		changed = RewriteCTECandidates(root, child, updater) || changed;
	}
	return RewriteCTECandidate(root, op, updater) || changed;
}

bool UnnestRewriter::RewriteInlineCTEDedupCandidate(unique_ptr<LogicalOperator> &root,
                                                    unique_ptr<LogicalOperator> &candidate,
                                                    UnnestRewriterPlanUpdater &updater) {
	auto &domain_cte = candidate->Cast<LogicalMaterializedCTE>();
	if (CountCTERefs(*candidate, domain_cte.table_index) != 2) {
		return false;
	}

	idx_t topmost_depth = 0;
	auto topmost_ptr = &domain_cte.children[1];
	while (topmost_ptr->get()->type == LogicalOperatorType::LOGICAL_PROJECTION &&
	       topmost_ptr->get()->children.size() == 1 &&
	       topmost_ptr->get()->children[0]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		topmost_ptr = &topmost_ptr->get()->children[0];
		topmost_depth++;
	}
	auto &topmost_op = *topmost_ptr->get();
	if (!IsSupportedUnnestTop(topmost_op.type) || topmost_op.children.size() != 1 ||
	    topmost_op.children[0]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}

	auto &join = topmost_op.children[0]->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER || join.conditions.size() != 1 || join.children.size() != 2) {
		return false;
	}

	auto domain_side = FindCTERefSide(join, domain_cte.table_index);
	if (!domain_side.IsValid()) {
		return false;
	}
	idx_t other_side = 1 - domain_side.GetIndex();
	auto domain_ref_bindings = join.children[domain_side.GetIndex()]->GetColumnBindings();
	if (join.children[other_side]->type == LogicalOperatorType::LOGICAL_GET &&
	    !ConvertCTETableInOutUnnest(context, root, join.children[other_side], domain_cte.table_index, false)) {
		return false;
	}

	vector<reference<unique_ptr<LogicalOperator>>> path_to_unnest;
	reference<unique_ptr<LogicalOperator>> current_op = join.children[other_side];
	while (current_op.get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		if (current_op.get()->children.size() != 1) {
			return false;
		}
		path_to_unnest.push_back(current_op);
		current_op = current_op.get()->children[0];
	}
	if (current_op.get()->type != LogicalOperatorType::LOGICAL_UNNEST) {
		return false;
	}
	auto &unnest = current_op.get()->Cast<LogicalUnnest>();
	if (unnest.children.size() != 1) {
		return false;
	}

	vector<ColumnBinding> candidate_delim_columns;
	if (!GetInlineDedupColumns(domain_cte, *unnest.children[0], candidate_delim_columns)) {
		return false;
	}
	auto dedup_bindings = unnest.children[0]->GetColumnBindings();
	if (dedup_bindings.size() != candidate_delim_columns.size()) {
		return false;
	}

	delim_columns = std::move(candidate_delim_columns);
	GetLHSExpressions(*domain_cte.children[0]);
	if (domain_ref_bindings.size() != lhs_bindings.size()) {
		delim_columns.clear();
		lhs_bindings.clear();
		return false;
	}

	// After replacing the CTE refs by the shared domain producer, expressions in the continuation must
	// read directly from the producer bindings. Projection pullup can leave projections above a pushed
	// filter/join; those still reference the domain CTE scan and must be rewritten with the join subtree.
	// The dedup bindings are retargeted to their original delimiter columns.
	ColumnBindingReplacer domain_ref_replacer;
	for (idx_t binding_idx = 0; binding_idx < lhs_bindings.size(); binding_idx++) {
		domain_ref_replacer.replacement_bindings.emplace_back(
		    domain_ref_bindings[binding_idx], lhs_bindings[binding_idx].binding, lhs_bindings[binding_idx].type);
	}
	for (idx_t binding_idx = 0; binding_idx < dedup_bindings.size(); binding_idx++) {
		domain_ref_replacer.replacement_bindings.emplace_back(dedup_bindings[binding_idx], delim_columns[binding_idx]);
	}
	domain_ref_replacer.VisitOperator(*domain_cte.children[1]);
	overwritten_tbl_idx = dedup_bindings[0].table_index;
	// Inline CTE dedup inputs can contain repeated source columns, e.g. table-in-out UNNEST can project an input
	// column and carry it again as a delimiter column. The RHS projection path only has the unique delimiter
	// columns at its tail.
	distinct_unnest_count = CountUniqueBindings(delim_columns);

	unnest.children[0] = std::move(domain_cte.children[0]);
	if (path_to_unnest.empty()) {
		updater.replace_bindings.clear();
		for (idx_t binding_idx = 0; binding_idx < dedup_bindings.size(); binding_idx++) {
			updater.replace_bindings.emplace_back(dedup_bindings[binding_idx], delim_columns[binding_idx]);
		}
		for (auto &unnest_expr : unnest.expressions) {
			updater.VisitExpression(&unnest_expr);
		}
		updater.replace_bindings.clear();
		topmost_op.children[0] = std::move(current_op.get());
		candidate = std::move(domain_cte.children[1]);
		delim_columns.clear();
		lhs_bindings.clear();
		return true;
	}
	topmost_op.children[0] = std::move(path_to_unnest.front().get());

	candidate = std::move(domain_cte.children[1]);
	reference<unique_ptr<LogicalOperator>> rewritten_topmost_ptr = candidate;
	for (idx_t depth = 0; depth < topmost_depth; depth++) {
		rewritten_topmost_ptr = rewritten_topmost_ptr.get()->children[0];
	}

	updater.overwritten_tbl_idx = overwritten_tbl_idx;
	UpdateBoundUnnestBindings(updater, rewritten_topmost_ptr.get());
	UpdateRHSBindings(root, rewritten_topmost_ptr.get(), updater);

	delim_columns.clear();
	lhs_bindings.clear();
	return true;
}

bool UnnestRewriter::RewriteCTECandidate(unique_ptr<LogicalOperator> &root, unique_ptr<LogicalOperator> &candidate,
                                         UnnestRewriterPlanUpdater &updater) {
	if (candidate->type != LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		return false;
	}
	auto &domain_cte = candidate->Cast<LogicalMaterializedCTE>();
	if (domain_cte.materialize != CTEMaterialize::CTE_MATERIALIZE_DEFAULT || domain_cte.children.size() != 2) {
		return false;
	}
	if (RewriteInlineCTEDedupCandidate(root, candidate, updater)) {
		return true;
	}
	if (domain_cte.children[1]->type != LogicalOperatorType::LOGICAL_PROJECTION ||
	    domain_cte.children[1]->children.size() != 1 ||
	    domain_cte.children[1]->children[0]->type != LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		return false;
	}

	auto &outer_projection = domain_cte.children[1]->Cast<LogicalProjection>();
	auto &dedup_cte = outer_projection.children[0]->Cast<LogicalMaterializedCTE>();
	if (dedup_cte.materialize != CTEMaterialize::CTE_MATERIALIZE_DEFAULT || dedup_cte.children.size() != 2) {
		return false;
	}
	if (dedup_cte.children[0]->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &dedup = dedup_cte.children[0]->Cast<LogicalAggregate>();
	if (dedup.children.size() != 1 || !IsCTERef(*dedup.children[0], domain_cte.table_index)) {
		return false;
	}

	domain_cte.children[0]->ResolveOperatorTypes();
	auto source_bindings = domain_cte.children[0]->GetColumnBindings();
	auto dedup_child_bindings = dedup.children[0]->GetColumnBindings();
	vector<ColumnBinding> candidate_delim_columns;
	for (auto &group : dedup.groups) {
		if (group->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &colref = group->Cast<BoundColumnRefExpression>();
		if (colref.Depth() != 0) {
			return false;
		}
		auto source_idx = FindBindingIndex(dedup_child_bindings, colref.Binding());
		if (!source_idx.IsValid() || source_idx.GetIndex() >= source_bindings.size()) {
			return false;
		}
		candidate_delim_columns.push_back(source_bindings[source_idx.GetIndex()]);
	}
	if (candidate_delim_columns.empty() || candidate_delim_columns.size() != dedup_cte.column_count) {
		return false;
	}

	if (CountCTERefs(*candidate, domain_cte.table_index) != 2 || CountCTERefs(*candidate, dedup_cte.table_index) != 1) {
		return false;
	}

	auto &topmost_op = *dedup_cte.children[1];
	if (!IsSupportedUnnestTop(topmost_op.type) || topmost_op.children.size() != 1 ||
	    topmost_op.children[0]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = topmost_op.children[0]->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER || join.conditions.size() != 1 || join.children.size() != 2) {
		return false;
	}

	auto domain_side = FindCTERefSide(join, domain_cte.table_index);
	if (!domain_side.IsValid()) {
		return false;
	}
	idx_t other_side = 1 - domain_side.GetIndex();

	if (join.children[other_side]->type == LogicalOperatorType::LOGICAL_GET &&
	    !ConvertCTETableInOutUnnest(context, root, join.children[other_side], dedup_cte.table_index)) {
		return false;
	}

	vector<reference<unique_ptr<LogicalOperator>>> path_to_unnest;
	reference<unique_ptr<LogicalOperator>> current_op = join.children[other_side];
	while (current_op.get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		if (current_op.get()->children.size() != 1) {
			return false;
		}
		path_to_unnest.push_back(current_op);
		current_op = current_op.get()->children[0];
	}
	if (path_to_unnest.empty() || current_op.get()->type != LogicalOperatorType::LOGICAL_UNNEST) {
		return false;
	}
	auto &unnest = current_op.get()->Cast<LogicalUnnest>();
	if (unnest.children.size() != 1 || !IsCTERef(*unnest.children[0], dedup_cte.table_index)) {
		return false;
	}
	auto &dedup_ref = unnest.children[0]->Cast<LogicalCTERef>();
	if (dedup_ref.chunk_types.size() != candidate_delim_columns.size()) {
		return false;
	}

	delim_columns = std::move(candidate_delim_columns);
	GetLHSExpressions(*domain_cte.children[0]);
	overwritten_tbl_idx = dedup_ref.table_index;
	distinct_unnest_count = dedup_ref.chunk_types.size();

	unnest.children[0] = std::move(domain_cte.children[0]);
	topmost_op.children[0] = std::move(path_to_unnest.front().get());

	updater.overwritten_tbl_idx = overwritten_tbl_idx;
	UpdateBoundUnnestBindings(updater, dedup_cte.children[1]);
	UpdateRHSBindings(root, dedup_cte.children[1], updater);

	auto replacement = std::move(domain_cte.children[1]);
	auto &replacement_projection = replacement->Cast<LogicalProjection>();
	auto &replacement_dedup_cte = replacement_projection.children[0]->Cast<LogicalMaterializedCTE>();
	replacement_projection.children[0] = std::move(replacement_dedup_cte.children[1]);
	candidate = std::move(replacement);

	delim_columns.clear();
	lhs_bindings.clear();
	return true;
}

bool UnnestRewriter::RewriteCandidate(unique_ptr<LogicalOperator> &candidate) {
	auto &topmost_op = *candidate;
	if (!IsSupportedUnnestTop(topmost_op.type)) {
		return false;
	}

	// get the LOGICAL_DELIM_JOIN, which is a child of the candidate
	D_ASSERT(topmost_op.children.size() == 1);
	auto &delim_join = topmost_op.children[0]->Cast<LogicalComparisonJoin>();
	D_ASSERT(delim_join.type == LogicalOperatorType::LOGICAL_DELIM_JOIN);
	GetDelimColumns(delim_join);

	// LHS of the LOGICAL_DELIM_JOIN is a LOGICAL_WINDOW that contains a LOGICAL_PROJECTION/LOGICAL_CROSS_JOIN
	// this lhs_proj later becomes the child of the UNNEST

	idx_t delim_idx = delim_join.delim_flipped ? 1 : 0;
	idx_t other_idx = 1 - delim_idx;
	auto &window = *delim_join.children[delim_idx];
	auto &lhs_op = window.children[0];
	GetLHSExpressions(*lhs_op);

	// find the LOGICAL_UNNEST
	// and get the path down to the LOGICAL_UNNEST
	vector<reference<unique_ptr<LogicalOperator>>> path_to_unnest;
	reference<unique_ptr<LogicalOperator>> curr_op = delim_join.children[other_idx];
	while (curr_op.get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		path_to_unnest.push_back(curr_op);
		curr_op = curr_op.get()->children[0];
	}

	// store the table index of the child of the LOGICAL_UNNEST
	// then update the plan by making the lhs_proj the child of the LOGICAL_UNNEST
	D_ASSERT(curr_op.get()->type == LogicalOperatorType::LOGICAL_UNNEST);
	auto &unnest = curr_op.get()->Cast<LogicalUnnest>();
	D_ASSERT(unnest.children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET);
	overwritten_tbl_idx = unnest.children[0]->Cast<LogicalDelimGet>().table_index;

	D_ASSERT(!unnest.children.empty());
	auto &delim_get = unnest.children[0]->Cast<LogicalDelimGet>();
	D_ASSERT(delim_get.chunk_types.size() > 1);
	distinct_unnest_count = delim_get.chunk_types.size();
	unnest.children[0] = std::move(lhs_op);

	// replace the LOGICAL_DELIM_JOIN with its RHS child operator
	topmost_op.children[0] = std::move(path_to_unnest.front().get());
	return true;
}

void UnnestRewriter::UpdateRHSBindings(unique_ptr<LogicalOperator> &plan, unique_ptr<LogicalOperator> &candidate,
                                       UnnestRewriterPlanUpdater &updater) {
	auto &topmost_op = *candidate;
	idx_t shift = lhs_bindings.size();

	vector<reference<unique_ptr<LogicalOperator>>> path_to_unnest;
	reference<unique_ptr<LogicalOperator>> curr_op = topmost_op.children[0];
	while (curr_op.get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		path_to_unnest.push_back(curr_op);
		curr_op = curr_op.get()->children[0];
	}

	// The trailing columns are duplicate-eliminated delimiter columns. References to those slots should
	// point at the prepended LHS columns after the rewrite, not at shifted payload columns.
	// Upper projections in the path reference the trailing slots of the projection below them,
	// so resolve bottom-up through the chain of trailing slots.
	column_binding_map_t<idx_t> trailing_slot_to_lhs;
	for (idx_t path_idx = path_to_unnest.size(); path_idx > 0; path_idx--) {
		auto &proj = path_to_unnest[path_idx - 1].get()->Cast<LogicalProjection>();
		D_ASSERT(proj.expressions.size() > distinct_unnest_count);
		auto payload_count = proj.expressions.size() - distinct_unnest_count;
		for (idx_t i = payload_count; i < proj.expressions.size(); i++) {
			auto &expr = proj.expressions[i];
			if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				continue;
			}
			auto &colref = expr->Cast<BoundColumnRefExpression>();
			auto removed_binding = colref.Binding();
			optional_idx found_lhs_idx;
			auto chained = trailing_slot_to_lhs.find(removed_binding);
			if (chained != trailing_slot_to_lhs.end()) {
				found_lhs_idx = chained->second;
			} else {
				if (removed_binding.table_index == overwritten_tbl_idx &&
				    removed_binding.column_index.GetIndex() < delim_columns.size()) {
					// CTE-based rewrites can still reference the deduplicated input here; translate it back to the
					// corresponding domain producer column before finding the new LHS projection slot.
					removed_binding = delim_columns[removed_binding.column_index.GetIndex()];
				}
				for (idx_t lhs_idx = 0; lhs_idx < lhs_bindings.size(); lhs_idx++) {
					if (removed_binding == lhs_bindings[lhs_idx].binding) {
						found_lhs_idx = lhs_idx;
						break;
					}
				}
			}
			if (found_lhs_idx.IsValid()) {
				ColumnBinding source_binding(proj.table_index, ProjectionIndex(i));
				ColumnBinding target_binding(proj.table_index, ProjectionIndex(found_lhs_idx.GetIndex()));
				updater.replace_bindings.emplace_back(source_binding, target_binding);
				trailing_slot_to_lhs[source_binding] = found_lhs_idx.GetIndex();
			}
		}
	}

	// drop the trailing delimiter columns and store all shifted payload bindings
	for (auto &path_op : path_to_unnest) {
		auto &proj = path_op.get()->Cast<LogicalProjection>();
		auto payload_count = proj.expressions.size() - distinct_unnest_count;
		proj.expressions.resize(payload_count);
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			ColumnBinding source_binding(proj.table_index, ProjectionIndex(i));
			ColumnBinding target_binding(proj.table_index, ProjectionIndex(i + shift));
			updater.replace_bindings.emplace_back(source_binding, target_binding);
		}
	}

	// update all bindings by shifting them
	updater.VisitOperator(*plan);
	updater.replace_bindings.clear();

	// update all bindings coming from the LHS to RHS bindings
	D_ASSERT(topmost_op.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
	auto &top_proj = topmost_op.children[0]->Cast<LogicalProjection>();
	vector<ColumnBinding> source_lhs_bindings;
	source_lhs_bindings.reserve(lhs_bindings.size());
	for (idx_t i = 0; i < lhs_bindings.size(); i++) {
		source_lhs_bindings.push_back(lhs_bindings[i].binding);
	}
	// References above the RHS projection path should point at the LHS columns exposed by the top projection.
	// Expressions inside the path itself still need to bind against their child; those are repaired below as each
	// projection gets the LHS columns prepended.
	ColumnBindingReplacer lhs_replacer;
	lhs_replacer.stop_operator = topmost_op.children[0];
	for (idx_t i = 0; i < lhs_bindings.size(); i++) {
		lhs_replacer.replacement_bindings.emplace_back(
		    lhs_bindings[i].binding, ColumnBinding(top_proj.table_index, ProjectionIndex(i)), lhs_bindings[i].type);
	}
	lhs_replacer.VisitOperator(*plan);

	// add the LHS expressions to each LOGICAL_PROJECTION
	for (idx_t i = path_to_unnest.size(); i > 0; i--) {
		D_ASSERT(path_to_unnest[i - 1].get()->type == LogicalOperatorType::LOGICAL_PROJECTION);
		auto &proj = path_to_unnest[i - 1].get()->Cast<LogicalProjection>();

		// temporarily store the existing expressions
		auto existing_expressions = std::move(proj.expressions);
		proj.expressions.clear();

		ColumnBindingReplacer child_lhs_replacer;
		for (idx_t expr_idx = 0; expr_idx < lhs_bindings.size(); expr_idx++) {
			if (source_lhs_bindings[expr_idx] == lhs_bindings[expr_idx].binding) {
				continue;
			}
			child_lhs_replacer.replacement_bindings.emplace_back(
			    source_lhs_bindings[expr_idx], lhs_bindings[expr_idx].binding, lhs_bindings[expr_idx].type);
		}
		for (auto &expr : existing_expressions) {
			child_lhs_replacer.VisitExpression(&expr);
		}

		// add the new expressions
		for (idx_t expr_idx = 0; expr_idx < lhs_bindings.size(); expr_idx++) {
			auto new_expr = make_uniq<BoundColumnRefExpression>(
			    lhs_bindings[expr_idx].alias, lhs_bindings[expr_idx].type, lhs_bindings[expr_idx].binding);
			proj.expressions.push_back(std::move(new_expr));

			// update the table index
			lhs_bindings[expr_idx].binding.table_index = proj.table_index;
			lhs_bindings[expr_idx].binding.column_index = ProjectionIndex(expr_idx);
		}

		// add the existing expressions again
		for (auto &expr : existing_expressions) {
			proj.expressions.push_back(std::move(expr));
		}

		proj.ResolveOperatorTypes();
	}
}

void UnnestRewriter::UpdateBoundUnnestBindings(UnnestRewriterPlanUpdater &updater,
                                               unique_ptr<LogicalOperator> &candidate) {
	auto &topmost_op = *candidate;

	// traverse LOGICAL_PROJECTION(s)
	reference<unique_ptr<LogicalOperator>> curr_op = topmost_op.children[0];
	while (curr_op.get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		curr_op = curr_op.get()->children[0];
	}

	// found the LOGICAL_UNNEST
	D_ASSERT(curr_op.get()->type == LogicalOperatorType::LOGICAL_UNNEST);
	auto &unnest = curr_op.get()->Cast<LogicalUnnest>();

	D_ASSERT(unnest.children.size() == 1);
	auto unnest_cols = unnest.children[0]->GetColumnBindings();

	for (idx_t i = 0; i < delim_columns.size(); i++) {
		auto delim_binding = delim_columns[i];

		auto unnest_it = unnest_cols.begin();
		while (unnest_it != unnest_cols.end()) {
			auto unnest_binding = *unnest_it;

			if (delim_binding.table_index == unnest_binding.table_index) {
				unnest_binding.table_index = overwritten_tbl_idx;
				unnest_binding.column_index = ProjectionIndex(i);
				updater.replace_bindings.emplace_back(unnest_binding, delim_binding);
				unnest_cols.erase(unnest_it);
				break;
			}
			unnest_it++;
		}
	}

	// update bindings
	for (auto &unnest_expr : unnest.expressions) {
		updater.VisitExpression(&unnest_expr);
	}
	updater.replace_bindings.clear();
}

void UnnestRewriter::GetDelimColumns(LogicalOperator &op) {
	D_ASSERT(op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN);
	auto &delim_join = op.Cast<LogicalComparisonJoin>();
	for (idx_t i = 0; i < delim_join.duplicate_eliminated_columns.size(); i++) {
		auto &expr = *delim_join.duplicate_eliminated_columns[i];
		D_ASSERT(expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF);
		auto &bound_colref_expr = expr.Cast<BoundColumnRefExpression>();
		delim_columns.push_back(bound_colref_expr.Binding());
	}
}

void UnnestRewriter::GetLHSExpressions(LogicalOperator &op) {
	op.ResolveOperatorTypes();
	auto col_bindings = op.GetColumnBindings();
	D_ASSERT(op.types.size() == col_bindings.size());

	bool set_alias = false;
	// we can easily extract the alias for LOGICAL_PROJECTION(s)
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = op.Cast<LogicalProjection>();
		if (proj.expressions.size() == op.types.size()) {
			set_alias = true;
		}
	}

	for (idx_t i = 0; i < op.types.size(); i++) {
		lhs_bindings.emplace_back(col_bindings[i], op.types[i]);
		if (set_alias) {
			auto &proj = op.Cast<LogicalProjection>();
			lhs_bindings.back().alias = proj.expressions[i]->GetAlias();
		}
	}
}

} // namespace duckdb
