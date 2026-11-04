#include "duckdb/planner/expression_binder/base_select_binder.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"

namespace duckdb {

BaseSelectBinder::BaseSelectBinder(Binder &binder, ClientContext &context, BoundSelectNode &node)
    : ExpressionBinder(binder, context), node(node) {
}

BindResult BaseSelectBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	if (reconstructing_generated_column && expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto dependency = binder.bind_context.GetGeneratedColumnDependency(expr.Cast<ColumnRefExpression>());
		if (dependency) {
			auto stack_checker = StackCheck(expr);
			return BindExpression(dependency, depth, root_expression);
		}
	}
	// check if the expression binds to one of the groups
	auto group_index = TryBindGroup(expr);
	if (group_index.IsValid()) {
		return BindGroup(expr, depth, group_index);
	}
	if (!inside_aggregate && !reconstructing_generated_column &&
	    expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto result = TryBindGeneratedColumn(expr.Cast<ColumnRefExpression>(), depth);
		if (result.expression) {
			return result;
		}
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
		if (inside_aggregate) {
			return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
		}
		return BindColumnRef(expr_ptr, depth, root_expression);
	case ExpressionClass::DEFAULT:
		return BindResult(BinderException::Unsupported(expr, "SELECT clause cannot contain DEFAULT clause"));
	case ExpressionClass::WINDOW:
		return BindWindowExpression(expr.Cast<WindowExpression>(), depth);
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
}

BindResult BaseSelectBinder::TryBindGeneratedColumn(ColumnRefExpression &expr, idx_t depth) {
	// Without grouping keys, only a constant NULL can bypass the source projection.
	auto expression = binder.bind_context.GetGeneratedColumnExpression(expr, node.groups.group_expressions.empty());
	if (!expression) {
		return BindResult();
	}
	BaseSelectBinder generated_binder(binder, context, node);
	generated_binder.reconstructing_generated_column = true;
	auto result = generated_binder.BindExpression(expression, depth);
	if (result.HasError() || generated_binder.HasBoundColumns()) {
		return BindResult();
	}
	result.expression->SetAlias(expr.GetAlias());
	return result;
}

bool BaseSelectBinder::ClaimsAlias(ColumnRefExpression &colref) {
	if (!ExpressionBinder::IsPotentialAlias(colref)) {
		return false;
	}
	auto &alias_map = node.bind_state.alias_map;
	return alias_map.find(colref.ColumnNames().back()) != alias_map.end();
}

bool BaseSelectBinder::HasCompoundGroups() {
	return node.bind_state.has_compound_groups;
}

ProjectionIndex BaseSelectBinder::FindGroup(ParsedExpression &expr) {
	if (node.bind_state.group_map.empty() && node.bind_state.group_alias_map.empty()) {
		return ProjectionIndex();
	}
	// the groups are keyed by the qualified form, so qualify against this scope before matching
	auto qualified = expr.Copy();
	ExpressionBinder::QualifyColumnNames(binder, qualified);
	return TryBindGroup(*qualified);
}

bool BaseSelectBinder::MatchesGroup(ParsedExpression &expr) {
	return FindGroup(expr).IsValid();
}

bool BaseSelectBinder::MatchesGroupWithoutSubquery(ParsedExpression &expr) {
	auto group_index = FindGroup(expr);
	auto &groups = node.groups.group_expressions;
	if (!group_index.IsValid() || group_index.GetIndex() >= groups.size()) {
		return false;
	}
	return !groups[group_index]->HasSubquery();
}

ProjectionIndex BaseSelectBinder::TryBindGroup(ParsedExpression &expr) {
	if (inside_aggregate || node.bind_state.unbound_groups.empty()) {
		return ProjectionIndex();
	}
	// first check the group alias map, if expr is a ColumnRefExpression
	auto &alias_map = node.bind_state.group_alias_map;
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!colref.IsQualified()) {
			auto alias_entry = alias_map.find(colref.ColumnNames()[0]);
			if (alias_entry != alias_map.end()) {
				// found entry!
				return alias_entry->second;
			}
		}
	}
	// no alias reference found
	// check the list of group columns for a match
	auto &group_map = node.bind_state.group_map;
	if (!reconstructing_generated_column) {
		auto entry = group_map.find(expr);
		if (entry != group_map.end()) {
			return entry->second;
		}
	}
	if (reconstructing_generated_column) {
		// Original grouping keys take precedence over equivalent generated definitions.
		for (idx_t i = 0; i < node.bind_state.unbound_groups.size(); i++) {
			auto &group = *node.bind_state.unbound_groups[i];
			if (binder.bind_context.MatchesGeneratedExpression(expr, group, true, false)) {
				return ProjectionIndex(i);
			}
		}
	}
	auto &candidates = node.bind_state.generated_group_candidates;
	for (idx_t index = candidates.size(); index > 0; index--) {
		auto group_index = candidates[index - 1];
		auto &group = *node.bind_state.unbound_groups[group_index.GetIndex()];
		if (binder.bind_context.MatchesGeneratedExpression(expr, group, reconstructing_generated_column, true)) {
			return group_index;
		}
	}
#ifdef DEBUG
	if (!reconstructing_generated_column) {
		for (auto map_entry : group_map) {
			D_ASSERT(!map_entry.first.get().Equals(expr));
			D_ASSERT(!expr.Equals(map_entry.first.get()));
		}
	}
#endif
	return ProjectionIndex();
}

BindResult BaseSelectBinder::BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (reconstructing_generated_column) {
		auto &colref = expr_ptr->Cast<ColumnRefExpression>();
		auto lambda_ref = colref.IsQualified()
		                      ? nullptr
		                      : LambdaRefExpression::FindMatchingBinding(lambda_bindings, colref.GetColumnName());
		if (!lambda_ref) {
			// Reject before binding can add columns to the source projection.
			return BindResult(BinderException(colref, "Cannot reconstruct an ungrouped column"));
		}
	}
	return ExpressionBinder::BindExpression(expr_ptr, depth);
}

BindResult BaseSelectBinder::BindGroupingFunction(OperatorExpression &op, idx_t depth) {
	if (node.groups.group_expressions.empty()) {
		return BindResult(BinderException(op, "GROUPING statement cannot be used without groups"));
	}
	vector<ProjectionIndex> group_indexes;
	if (op.GetChildren().empty()) {
		// No arguments provided - use all group columns
		for (idx_t i = 0; i < node.groups.group_expressions.size(); i++) {
			group_indexes.push_back(ProjectionIndex(i));
		}
	} else {
		for (auto &child : op.GetChildrenMutable()) {
			ExpressionBinder::QualifyColumnNames(binder, child);
			auto idx = TryBindGroup(*child);
			if (!idx.IsValid()) {
				return BindResult(BinderException(op, "GROUPING child %s must be a grouping column", child->GetName()));
			}
			group_indexes.push_back(idx);
		}
	}
	if (group_indexes.size() >= 64) {
		return BindResult(BinderException(op, "GROUPING statement cannot have more than 64 groups"));
	}
	ProjectionIndex col_idx(node.grouping_functions.size());
	node.grouping_functions.push_back(std::move(group_indexes));
	return BindResult(make_uniq<BoundColumnRefExpression>(Identifier(op.GetName()), LogicalType::BIGINT,
	                                                      ColumnBinding(node.groupings_index, col_idx), depth));
}

BindResult BaseSelectBinder::BindGroup(ParsedExpression &expr, idx_t depth, ProjectionIndex group_index) {
	auto &collated_groups = node.bind_state.collated_groups;
	auto it = collated_groups.find(group_index);
	if (it != collated_groups.end()) {
		// This is an implicitly collated group, so we need to refer to the first() aggregate
		const auto &aggr_index = it->second;
		const auto return_type = node.aggregates[aggr_index]->GetReturnType();
		auto uncollated_first_expression = make_uniq<BoundColumnRefExpression>(
		    Identifier(expr.GetName()), return_type, ColumnBinding(node.aggregate_index, aggr_index), depth);

		if (node.groups.grouping_sets.size() <= 1) {
			// if there are no more than two grouping sets, you can return the uncollated first expression.
			// "first" meaning the aggregate function.
			return BindResult(std::move(uncollated_first_expression));
		}

		// otherwise we insert a case statement to return NULL when the collated group expression is NULL
		// otherwise you can return the "first" of the uncollated expression.
		auto &group = node.groups.group_expressions[group_index];
		auto collated_group_expression = make_uniq<BoundColumnRefExpression>(
		    Identifier(expr.GetName()), group->GetReturnType(), ColumnBinding(node.group_index, group_index), depth);

		auto sql_null = make_uniq<BoundConstantExpression>(Value(return_type));
		auto when_expr = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
		when_expr->GetChildrenMutable().push_back(std::move(collated_group_expression));
		auto then_expr = make_uniq<BoundConstantExpression>(Value(return_type));
		auto else_expr = std::move(uncollated_first_expression);
		auto case_expr =
		    make_uniq<BoundCaseExpression>(std::move(when_expr), std::move(then_expr), std::move(else_expr));
		return BindResult(std::move(case_expr));
	} else {
		auto &group = node.groups.group_expressions[group_index];
		return BindResult(make_uniq<BoundColumnRefExpression>(Identifier(expr.GetName()), group->GetReturnType(),
		                                                      ColumnBinding(node.group_index, group_index), depth));
	}
}

} // namespace duckdb
