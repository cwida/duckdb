#include "duckdb/optimizer/filter_pullup.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> FilterPullup::Rewrite(unique_ptr<LogicalOperator> op) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_FILTER:
		return PullupFilter(std::move(op));
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return PullupProjection(std::move(op));
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return PullupCrossProduct(std::move(op));
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		return PullupJoin(std::move(op));
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_EXCEPT:
		return PullupSetOperation(std::move(op));
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return PullupDistinct(std::move(op));
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		// we can just pull directly through these operations without any rewriting
		op->children[0] = Rewrite(std::move(op->children[0]));
		return op;
	}
	default:
		return FinishPullup(std::move(op));
	}
}

unique_ptr<LogicalOperator> FilterPullup::PullupJoin(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN || op->type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN);
	auto &join = op->Cast<LogicalJoin>();

	switch (join.join_type) {
	case JoinType::INNER:
		if (op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			//	We can only move filters through the left side of AsOf joins.
			return PullupFromLeft(std::move(op));
		} else {
			return PullupInnerJoin(std::move(op));
		}
	case JoinType::LEFT:
	case JoinType::ANTI:
	case JoinType::SEMI: {
		return PullupFromLeft(std::move(op));
	}
	default:
		// unsupported join type: call children pull up
		return FinishPullup(std::move(op));
	}
}

unique_ptr<LogicalOperator> FilterPullup::PullupInnerJoin(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->Cast<LogicalJoin>().join_type == JoinType::INNER);
	if (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return op;
	}
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_ANY_JOIN);

	// Get the filters from both sides of the join
	op = PullupBothSide(std::move(op));
	vector<unique_ptr<Expression>> expressions;
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		expressions = std::move(op->expressions);
		op = std::move(op->children[0]);
	} else if (!can_pullup) {
		return op; // No filters from below, and we can't pullup, stop.
	}

	// Also extract the filters of the joins
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &comp_join = op->Cast<LogicalComparisonJoin>();
		for (auto &cond : comp_join.conditions) {
			expressions.push_back(JoinCondition::CreateExpression(std::move(cond)));
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &any_join = op->Cast<LogicalAnyJoin>();
		expressions.push_back(std::move(any_join.condition));
		break;
	}
	default:
		throw NotImplementedException("PullupInnerJoin for LogicalOperatorType::%s", EnumUtil::ToString(op->type));
	}

	// Convert to cross product
	op = make_uniq<LogicalCrossProduct>(std::move(op->children[0]), std::move(op->children[1]));
	if (can_pullup) {
		for (auto &expr : expressions) {
			filters_expr_pullup.push_back(std::move(expr));
		}
	} else {
		op = GeneratePullupFilter(std::move(op), expressions);
	}
	return op;
}

unique_ptr<LogicalOperator> FilterPullup::PullupCrossProduct(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	return PullupBothSide(std::move(op));
}

unique_ptr<LogicalOperator> FilterPullup::PullupDistinct(unique_ptr<LogicalOperator> op) {
	const auto &distinct = op->Cast<LogicalDistinct>();
	if (distinct.distinct_type == DistinctType::DISTINCT) {
		// Can pull up through a DISTINCT
		op->children[0] = Rewrite(std::move(op->children[0]));
		for (auto &expr : filters_expr_pullup) {
			if (expr->IsVolatile()) {
				// Keep volatile predicates below the DISTINCT, where they run once per input row
				op->children[0] = GeneratePullupFilter(std::move(op->children[0]), filters_expr_pullup);
				return op;
			}
		}
		return op;
	}
	// Cannot pull up through a DISTINCT ON (see #19327)
	D_ASSERT(distinct.distinct_type == DistinctType::DISTINCT_ON);
	return FinishPullup(std::move(op));
}

unique_ptr<LogicalOperator> FilterPullup::GeneratePullupFilter(unique_ptr<LogicalOperator> child,
                                                               vector<unique_ptr<Expression>> &expressions) {
	unique_ptr<LogicalFilter> filter = make_uniq<LogicalFilter>();
	for (idx_t i = 0; i < expressions.size(); ++i) {
		filter->expressions.push_back(std::move(expressions[i]));
	}
	expressions.clear();
	filter->children.push_back(std::move(child));
	return std::move(filter);
}

unique_ptr<LogicalOperator> FilterPullup::FinishPullup(unique_ptr<LogicalOperator> op) {
	// unhandled type, first perform filter pushdown in its children
	for (idx_t i = 0; i < op->children.size(); i++) {
		FilterPullup pullup;
		op->children[i] = pullup.Rewrite(std::move(op->children[i]));
	}
	// now pull up any existing filters
	if (filters_expr_pullup.empty()) {
		// no filters to pull up
		return op;
	}
	return GeneratePullupFilter(std::move(op), filters_expr_pullup);
}

} // namespace duckdb
