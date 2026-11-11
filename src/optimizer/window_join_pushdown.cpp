#include "duckdb/optimizer/window_join_pushdown.hpp"

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder/base_select_binder.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/optimizer/apply_functional_dependencies.hpp"

//	From https://www.vldb.org/pvldb/vol19/p3525-lindner.pdf §5 (Partition Analysis)
//	Join push-down P,O,w,f references only A(r) and:
// 	(9)  ⊞P,...(r)⋈︁r.b=s.x s ≡⊞P,...(r ⋈︁r.b=s.x s) | FD P → b and x unique
// 	(10) ⊞P,...(r)⋈︁r.b=s.x s ≡⊞P,...(r ⋈︁r.b=s.x s) | FD P → b and x unique (left join)
//  (11) ⊞P,...(r)⋉r.b=s.x s ≡⊞P,...(r ⋉r.b=s.x s) | FD P → b
//	(12) ⊞P,...(r)⊲r.b=s.x s ≡⊞P,...(r ⊲r.b=s.x s) | FD P → b
//
//	Note that the table in the paper is incorrect: equivalence 10 in the table only requires x unique,
//	but §5.2 argues:
//	> Equivalences 7 and 9 to 12 require the FD P → b.
//
//	Note also that the uniqueness condition is at the end of that section:
//	> Inner and outer joins (Equivalences 9 and 10) can expand it,
//	> so an engine should push them only when the join does not grow the input it feeds the window.

namespace duckdb {

bool WindowJoinPushdown::CanOptimize(const LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	const auto &join = op.Cast<LogicalComparisonJoin>();

	//	Single equality join
	if (join.conditions.size() != 1) {
		return false;
	}
	const auto &condition = join.conditions[0];
	if (!condition.IsComparison()) {
		return false;
	}
	switch (condition.GetComparisonType()) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		break;
	default:
		return false;
	}

	//	Find the window side
	idx_t window_idx = 0;
	switch (join.join_type) {
	case JoinType::INNER:
		if (join.children[window_idx]->type != LogicalOperatorType::LOGICAL_WINDOW) {
			++window_idx;
		}
		//	TODO: Push down stacked window joins?
		if (join.children[1 - window_idx]->type == LogicalOperatorType::LOGICAL_WINDOW) {
			return false;
		}
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case JoinType::LEFT:
	case JoinType::SEMI:
	case JoinType::ANTI:
		//	Only inner joins can be symmetric
		if (join.children[window_idx]->type != LogicalOperatorType::LOGICAL_WINDOW) {
			return false;
		}
		break;
	default:
		return false;
	}
	const auto &window = join.children[window_idx]->Cast<LogicalWindow>();
	const auto &b = (window_idx == 0) ? condition.LeftReference() : condition.RightReference();

	//	FD P → b
	for (const auto &expr : window.expressions) {
		const auto &wexpr = expr->Cast<BoundWindowExpression>();
		vector<reference<Expression>> refs;
		for (auto &p : wexpr.Partitions()) {
			refs.emplace_back(*p);
		}
		if (refs.empty()) {
			return false;
		}
		if (!BaseSelectBinder::IsFunctionallyDependent(b, refs)) {
			return false;
		}
	}

	switch (join.join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
		return true;
	default:
		break;
	}

	//	x unique
	const auto &x = (window_idx == 1) ? condition.GetLHS() : condition.GetRHS();
	if (x.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		//	Need a bound reference here
		const auto &binding = x.Cast<BoundColumnRefExpression>().Binding();
		auto &s = *join.children[1 - window_idx];
		const auto uccs = ApplyFunctionalDependencies::GetUniqueColumnCombinations(s);
		for (const auto &ucc : uccs) {
			if (ucc.size() != 1 || ucc.count(binding) == 0) {
				continue;
			}
			return true;
		}
	}

	return false;
}

unique_ptr<LogicalOperator> WindowJoinPushdown::OptimizeInternal(unique_ptr<LogicalOperator> op) {
	if (!CanOptimize(*op)) {
		for (auto &child : op->children) {
			child = OptimizeInternal(std::move(child));
		}
		return op;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();

	idx_t window_idx = 0;
	if (join.join_type == JoinType::INNER && join.children[window_idx]->type != LogicalOperatorType::LOGICAL_WINDOW) {
		++window_idx;
	}
	auto &window = join.children[window_idx]->Cast<LogicalWindow>();

	auto new_join = make_uniq<LogicalComparisonJoin>(join.join_type, join.type);
	for (auto &condition : join.conditions) {
		new_join->conditions.emplace_back(std::move(condition));
	}
	if (window_idx) {
		new_join->children.emplace_back(std::move(join.children[0]));
		new_join->children.emplace_back(std::move(window.children[0]));
	} else {
		new_join->children.emplace_back(std::move(window.children[0]));
		new_join->children.emplace_back(std::move(join.children[1]));
	}

	auto new_window = make_uniq<LogicalWindow>(window.window_index);
	new_window->children.emplace_back(std::move(new_join));
	for (auto &expr : window.expressions) {
		new_window->expressions.emplace_back(std::move(expr));
	}

	return std::move(new_window);
}

unique_ptr<LogicalOperator> WindowJoinPushdown::Optimize(unique_ptr<LogicalOperator> op) {
	return OptimizeInternal(std::move(op));
}

} // namespace duckdb
