#include "duckdb/planner/joinside.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

JoinCondition JoinCondition::Copy() const {
	if (IsComparison()) {
		JoinCondition copy(left->Copy(), right->Copy(), comparison);
		if (left_stats) {
			copy.SetLeftStats(left_stats->ToUnique());
		}
		if (right_stats) {
			copy.SetRightStats(right_stats->ToUnique());
		}
		return copy;
	}

	JoinCondition copy(left->Copy());
	const auto &expr_stats = GetExpressionStats();
	if (expr_stats) {
		copy.SetExpressionStats(expr_stats->ToUnique());
	}
	return copy;
}

unique_ptr<Expression> JoinCondition::CreateExpression(JoinCondition cond) {
	if (cond.IsComparison()) {
		auto bound_comparison = BoundComparisonExpression::Create(
		    cond.GetComparisonType(), std::move(cond.LeftReference()), std::move(cond.RightReference()));
		return bound_comparison;
	}
	return std::move(cond.JoinExpressionReference());
}

unique_ptr<Expression> JoinCondition::CreateExpression(vector<JoinCondition> conditions) {
	unique_ptr<Expression> result;
	for (auto &cond : conditions) {
		auto expr = CreateExpression(std::move(cond));
		if (!result) {
			result = std::move(expr);
		} else {
			auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
			                                                  std::move(expr));
			result = std::move(conj);
		}
	}
	return result;
}

JoinSide JoinSide::CombineJoinSide(JoinSide left, JoinSide right) {
	if (left == JoinSide::NONE) {
		return right;
	}
	if (right == JoinSide::NONE) {
		return left;
	}
	if (left != right) {
		return JoinSide::BOTH;
	}
	return left;
}

static JoinSide GetJoinSideInternal(TableIndex table_binding, const unordered_set<TableIndex> &left_bindings,
                                    const unordered_set<TableIndex> &right_bindings, bool current_scope_only) {
	if (left_bindings.find(table_binding) != left_bindings.end()) {
		D_ASSERT(right_bindings.find(table_binding) == right_bindings.end());
		return JoinSide::LEFT;
	}
	if (right_bindings.find(table_binding) != right_bindings.end()) {
		return JoinSide::RIGHT;
	}
	if (current_scope_only) {
		return JoinSide::NONE;
	}
	D_ASSERT(false);
	return JoinSide::RIGHT;
}

static JoinSide GetJoinSideInternal(const Expression &expression, const unordered_set<TableIndex> &left_bindings,
                                    const unordered_set<TableIndex> &right_bindings, bool current_scope_only) {
	if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expression.Cast<BoundColumnRefExpression>();
		if (colref.Depth() > 0) {
			return JoinSide::NONE;
		}
		return GetJoinSideInternal(colref.Binding().table_index, left_bindings, right_bindings, current_scope_only);
	}
	D_ASSERT(expression.GetExpressionType() != ExpressionType::BOUND_REF);
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subquery = expression.Cast<BoundSubqueryExpression>();
		JoinSide side = JoinSide::NONE;
		for (auto &child : subquery.GetChildren()) {
			auto child_side = GetJoinSideInternal(*child, left_bindings, right_bindings, current_scope_only);
			side = JoinSide::CombineJoinSide(side, child_side);
		}
		for (auto &corr : subquery.GetBinder()->correlated_columns) {
			if (corr.depth > 1) {
				if (current_scope_only) {
					continue;
				}
				return JoinSide::BOTH;
			}
			auto correlated_side =
			    GetJoinSideInternal(corr.binding.table_index, left_bindings, right_bindings, current_scope_only);
			side = JoinSide::CombineJoinSide(side, correlated_side);
		}
		return side;
	}
	JoinSide join_side = JoinSide::NONE;
	ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) {
		auto child_side = GetJoinSideInternal(child, left_bindings, right_bindings, current_scope_only);
		join_side = JoinSide::CombineJoinSide(child_side, join_side);
	});
	return join_side;
}

JoinSide JoinSide::GetJoinSide(TableIndex table_binding, const unordered_set<TableIndex> &left_bindings,
                               const unordered_set<TableIndex> &right_bindings) {
	return GetJoinSideInternal(table_binding, left_bindings, right_bindings, false);
}

JoinSide JoinSide::GetJoinSide(const Expression &expression, const unordered_set<TableIndex> &left_bindings,
                               const unordered_set<TableIndex> &right_bindings) {
	return GetJoinSideInternal(expression, left_bindings, right_bindings, false);
}

JoinSide JoinSide::GetCurrentJoinSide(const Expression &expression, const unordered_set<TableIndex> &left_bindings,
                                      const unordered_set<TableIndex> &right_bindings) {
	return GetJoinSideInternal(expression, left_bindings, right_bindings, true);
}

JoinSide JoinSide::GetJoinSide(const unordered_set<TableIndex> &bindings,
                               const unordered_set<TableIndex> &left_bindings,
                               const unordered_set<TableIndex> &right_bindings) {
	JoinSide side = JoinSide::NONE;
	for (auto binding : bindings) {
		side = CombineJoinSide(side, GetJoinSide(binding, left_bindings, right_bindings));
	}
	return side;
}

} // namespace duckdb
