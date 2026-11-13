#include "duckdb/optimizer/mark_join_to_semi.hpp"

#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

namespace duckdb {

namespace {

enum class MarkUse : uint8_t { NONE, MARK, NOT_MARK };

void AddReferencedTables(const Expression &expr, unordered_set<TableIndex> &referenced) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expr, [&](const BoundColumnRefExpression &colref) { referenced.insert(colref.Binding().table_index); });
}

bool ReferencesTable(const Expression &expr, TableIndex table_index) {
	unordered_set<TableIndex> referenced;
	AddReferencedTables(expr, referenced);
	return referenced.find(table_index) != referenced.end();
}

bool IsMark(const Expression &expr, TableIndex mark_index) {
	return expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
	       expr.Cast<BoundColumnRefExpression>().Binding().table_index == mark_index;
}

//! Whether the expression is the bare mark column or NOT(mark)
MarkUse GetMarkUse(const Expression &expr, TableIndex mark_index) {
	if (IsMark(expr, mark_index)) {
		return MarkUse::MARK;
	}
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		if (children.size() == 1 && IsMark(*children[0], mark_index)) {
			return MarkUse::NOT_MARK;
		}
	}
	return MarkUse::NONE;
}

bool ReadsDelimGet(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	for (auto &child : op.children) {
		if (ReadsDelimGet(*child)) {
			return true;
		}
	}
	return false;
}

//! Whether the filter passes the columns of its child through unchanged
bool HasIdentityProjection(LogicalFilter &filter) {
	if (filter.projection_map.empty()) {
		return true;
	}
	auto child_column_count = filter.children[0]->GetColumnBindings().size();
	if (filter.projection_map.size() != child_column_count) {
		return false;
	}
	for (idx_t i = 0; i < filter.projection_map.size(); i++) {
		if (filter.projection_map[i].GetIndex() != i) {
			return false;
		}
	}
	return true;
}

} // namespace

MarkJoinToSemi::MarkJoinToSemi(Optimizer &optimizer) : optimizer(optimizer) {
}

bool MarkJoinToSemi::Optimize(unique_ptr<LogicalOperator> &op) {
	changed = false;
	unordered_set<TableIndex> referenced;
	VisitOperator(op, referenced);
	return changed;
}

void MarkJoinToSemi::VisitOperator(unique_ptr<LogicalOperator> &op, const unordered_set<TableIndex> &referenced_p) {
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		TryConvert(op, referenced_p);
	}
	// collect the tables referenced by this operator: a mark that is read above its filter has to stay
	auto referenced = referenced_p;
	LogicalOperatorVisitor::EnumerateExpressions(
	    static_cast<const LogicalOperator &>(*op),
	    [&](const unique_ptr<Expression> *expression) { AddReferencedTables(**expression, referenced); });
	for (auto &child : op->children) {
		VisitOperator(child, referenced);
	}
}

void MarkJoinToSemi::TryConvert(unique_ptr<LogicalOperator> &op, const unordered_set<TableIndex> &referenced) {
	auto &filter = op->Cast<LogicalFilter>();
	auto &child = *filter.children[0];
	if (child.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return;
	}
	auto &join = child.Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::MARK || join.filter_pushdown) {
		return;
	}
	auto mark_index = join.mark_index;
	if (referenced.find(mark_index) != referenced.end()) {
		// the mark is read above the filter
		return;
	}
	if (ReadsDelimGet(*join.children[1])) {
		// the right side is still correlated
		return;
	}
	if (join.children[1]->type == LogicalOperatorType::LOGICAL_CHUNK_GET) {
		// IN list: the list is already pushed into the scan as a filter, a SEMI join would only repeat it
		return;
	}

	// find the filter expression that consumes the mark: no other expression is allowed to reference it
	optional_idx mark_position;
	auto mark_use = MarkUse::NONE;
	for (idx_t i = 0; i < filter.expressions.size(); i++) {
		auto &expr = *filter.expressions[i];
		auto expr_use = GetMarkUse(expr, mark_index);
		if (expr_use != MarkUse::NONE && !mark_position.IsValid()) {
			mark_position = i;
			mark_use = expr_use;
			continue;
		}
		if (ReferencesTable(expr, mark_index)) {
			return;
		}
	}
	if (!mark_position.IsValid()) {
		return;
	}
	if (mark_use == MarkUse::NOT_MARK) {
		// NOT(mark) is an ANTI join only if the mark is never NULL, i.e. if all conditions are null-safe
		for (auto &cond : join.conditions) {
			if (!cond.IsComparison()) {
				continue;
			}
			if (cond.GetComparisonType() != ExpressionType::COMPARE_DISTINCT_FROM &&
			    cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
				return;
			}
		}
	}
	// the mark is the last column of the join: the projection map of the filter stays valid if it does not select it
	auto mark_column = join.GetColumnBindings().size() - 1;
	for (auto &index : filter.projection_map) {
		if (index.GetIndex() == mark_column) {
			return;
		}
	}

	filter.expressions.erase_at(mark_position.GetIndex());
	if (mark_use == MarkUse::MARK) {
		join.join_type = JoinType::SEMI;
		FilterPushdown::SimplifyNullSafeSemiJoinConditions(optimizer.context, join);
	} else {
		join.join_type = JoinType::ANTI;
	}
	changed = true;
	if (filter.expressions.empty() && HasIdentityProjection(filter)) {
		// nothing left to filter or project: the join takes the place of the filter, including its estimated
		// cardinality (the estimate of the MARK join is the cardinality of its left side)
		if (filter.has_estimated_cardinality) {
			join.SetEstimatedCardinality(filter.estimated_cardinality);
		}
		op = std::move(filter.children[0]);
	}
	op->ResolveOperatorTypes();
}

} // namespace duckdb
