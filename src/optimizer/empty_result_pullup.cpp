#include "duckdb/optimizer/empty_result_pullup.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> EmptyResultPullup::PullUpEmptyJoinChildren(unique_ptr<LogicalOperator> op) {
	JoinType join_type = JoinType::INVALID;
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_ANY_JOIN || op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN || op->type == LogicalOperatorType::LOGICAL_EXCEPT);
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		join_type = op->Cast<LogicalComparisonJoin>().join_type;
		break;
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		join_type = op->Cast<LogicalAnyJoin>().join_type;
		break;
	case LogicalOperatorType::LOGICAL_EXCEPT:
		join_type = JoinType::ANTI;
		break;
	case LogicalOperatorType::LOGICAL_INTERSECT:
		join_type = JoinType::SEMI;
		break;
	default:
		break;
	}

	switch (join_type) {
	case JoinType::SEMI:
	case JoinType::INNER: {
		for (auto &child : op->children) {
			if (child->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
				op = make_uniq<LogicalEmptyResult>(std::move(op));
				break;
			}
		}
		break;
	}
	// For ANTI joins, if the right child is empty, the whole join collapses to the left child
	case JoinType::ANTI: {
		if (op->children[1]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT &&
		    op->type != LogicalOperatorType::LOGICAL_EXCEPT) {
			op = std::move(op->children[0]);
			break;
		}
		if (op->children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
			op = make_uniq<LogicalEmptyResult>(std::move(op));
		}
		break;
	}
	case JoinType::MARK:
	case JoinType::SINGLE:
	case JoinType::LEFT: {
		if (op->children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
			op = make_uniq<LogicalEmptyResult>(std::move(op));
		}
		break;
	}
	default:
		break;
	}
	return op;
}

unique_ptr<LogicalOperator> EmptyResultPullup::Optimize(unique_ptr<LogicalOperator> op) {
	for (idx_t i = 0; i < op->children.size(); i++) {
		op->children[i] = Optimize(std::move(op->children[i]));
	}
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_PIVOT:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		for (auto &child : op->children) {
			if (child->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
				op = make_uniq<LogicalEmptyResult>(std::move(op));
				break;
			}
		}
		return op;
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		D_ASSERT(op->children.size() == 2);
		if (op->children[1]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
			if (op->children[0]->HasSideEffects()) {
				return op;
			}
			op = make_uniq<LogicalEmptyResult>(std::move(op));
			break;
		}
		return op;
	}
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		op = PullUpEmptyJoinChildren(std::move(op));
		break;
	}
	default:
		break;
	}
	return op;
}

} // namespace duckdb
