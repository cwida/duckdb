#include "duckdb/optimizer/remove_redundant_order.hpp"

#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb {

namespace {

//! A hash aggregate sinks in parallel, so the input order never reaches its aggregates; an ungrouped aggregate sinks in
//! order when one of its aggregates reads the rows in order
bool AggregateDiscardsOrder(const LogicalAggregate &aggr) {
	if (!aggr.groups.empty()) {
		return true;
	}
	for (auto &expr : aggr.expressions) {
		auto &bound = expr->Cast<BoundAggregateExpression>();
		if (bound.GetOrderBys()) {
			// the aggregate sorts its own input
			continue;
		}
		if (bound.Function().GetOrderDependent() == AggregateOrderDependent::ORDER_DEPENDENT) {
			return false;
		}
	}
	return true;
}

//! DISTINCT ON keeps the first row of every group, so it reads its input in order; plain DISTINCT does not
bool DistinctDiscardsOrder(const LogicalDistinct &distinct) {
	return distinct.distinct_type == DistinctType::DISTINCT;
}

//! UNION ALL concatenates its inputs in order; the hash-based set operations do not
bool SetOperationDiscardsOrder(const LogicalSetOperation &setop) {
	return setop.type != LogicalOperatorType::LOGICAL_UNION || !setop.setop_all;
}

} // namespace

void RemoveRedundantOrder::Optimize(unique_ptr<LogicalOperator> &op) {
	// the order of the query result is observable
	Optimize(op, false);
}

void RemoveRedundantOrder::Optimize(unique_ptr<LogicalOperator> &op, bool order_discarded) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		if (order_discarded && !op->HasProjectionMap()) {
			// nothing above reads the order: drop the sort and look at its input in the same context
			auto child = std::move(op->children[0]);
			op = std::move(child);
			Optimize(op, true);
			return;
		}
		// a sort replaces whatever order its input had
		Optimize(op->children[0], true);
		return;
	}
	case LogicalOperatorType::LOGICAL_TOP_N:
		Optimize(op->children[0], true);
		return;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		Optimize(op->children[0], AggregateDiscardsOrder(op->Cast<LogicalAggregate>()));
		return;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		Optimize(op->children[0], DistinctDiscardsOrder(op->Cast<LogicalDistinct>()));
		return;
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		auto discards = order_discarded || SetOperationDiscardsOrder(op->Cast<LogicalSetOperation>());
		for (auto &child : op->children) {
			Optimize(child, discards);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		// joins do not preserve the order of either input
		for (auto &child : op->children) {
			Optimize(child, true);
		}
		return;
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_UNNEST:
		// row-wise operators neither read nor change the order: the operators above them decide
		Optimize(op->children[0], order_discarded);
		return;
	default:
		// everything else (LIMIT, window functions, samples, CTEs, DML sinks, ...) may read its input in order
		for (auto &child : op->children) {
			Optimize(child, false);
		}
		return;
	}
}

} // namespace duckdb
