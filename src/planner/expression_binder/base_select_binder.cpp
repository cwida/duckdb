#include "duckdb/planner/expression_binder/base_select_binder.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
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
	// check if the expression binds to one of the groups
	auto group_index = TryBindGroup(expr);
	if (group_index.IsValid()) {
		return BindGroup(expr, depth, group_index);
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
	if (inside_aggregate) {
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
	auto entry = group_map.find(expr);
	if (entry != group_map.end()) {
		return entry->second;
	}
#ifdef DEBUG
	for (auto map_entry : group_map) {
		D_ASSERT(!map_entry.first.get().Equals(expr));
		D_ASSERT(!expr.Equals(map_entry.first.get()));
	}
#endif
	return ProjectionIndex();
}

BindResult BaseSelectBinder::BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
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
