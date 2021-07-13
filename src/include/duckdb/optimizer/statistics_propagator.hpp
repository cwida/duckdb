//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/statistics_propagator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_tokens.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

namespace duckdb {
class ClientContext;
class LogicalOperator;
class TableFilter;

class StatisticsPropagator {
public:
	explicit StatisticsPropagator(ClientContext &context);

	unique_ptr<NodeStatistics> PropagateStatistics(unique_ptr<LogicalOperator> &node_ptr);

private:
	//! Propagate statistics through an operator
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalOperator &node, unique_ptr<LogicalOperator> *node_ptr);

	unique_ptr<NodeStatistics> PropagateStatistics(LogicalFilter &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalGet &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalJoin &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalProjection &op, unique_ptr<LogicalOperator> *node_ptr);
	void PropagateStatistics(LogicalComparisonJoin &op, unique_ptr<LogicalOperator> *node_ptr);
	void PropagateStatistics(LogicalAnyJoin &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalSetOperation &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalAggregate &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalCrossProduct &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalLimit &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalOrder &op, unique_ptr<LogicalOperator> *node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalWindow &op, unique_ptr<LogicalOperator> *node_ptr);

	unique_ptr<NodeStatistics> PropagateChildren(LogicalOperator &node, unique_ptr<LogicalOperator> *node_ptr);

	//! Return statistics from a constant value
	unique_ptr<BaseStatistics> StatisticsFromValue(const Value &input);
	//! Run a comparison with two sets of statistics, returns if the comparison will always returns true/false or not
	FilterPropagateResult PropagateComparison(BaseStatistics &left, BaseStatistics &right, ExpressionType comparison);

	//! Update filter statistics from a filter with a constant
	void UpdateFilterStatistics(BaseStatistics &input, ExpressionType comparison_type, const Value &constant);
	//! Update statistics from a filter between two stats
	void UpdateFilterStatistics(BaseStatistics &lstats, BaseStatistics &rstats, ExpressionType comparison_type);
	//! Update filter statistics from a generic comparison
	void UpdateFilterStatistics(Expression &left, Expression &right, ExpressionType comparison_type);
	//! Update filter statistics from an expression
	void UpdateFilterStatistics(Expression &condition);
	//! Set the statistics of a specific column binding to not contain null values
	void SetStatisticsNotNull(ColumnBinding binding);

	//! Run a comparison between the statistics and the table filter; returns the prune result
	FilterPropagateResult PropagateTableFilter(BaseStatistics &stats, TableFilter &filter);
	//! Update filter statistics from a TableFilter
	void UpdateFilterStatistics(BaseStatistics &input, TableFilter &filter);

	//! Add cardinalities together (i.e. new max is stats.max + new_stats.max): used for union
	void AddCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats);
	//! Multiply the cardinalities together (i.e. new max cardinality is stats.max * new_stats.max): used for
	//! joins/cross products
	void MultiplyCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats);

	unique_ptr<BaseStatistics> PropagateExpression(unique_ptr<Expression> &expr);
	unique_ptr<BaseStatistics> PropagateExpression(Expression &expr, unique_ptr<Expression> *expr_ptr);

	unique_ptr<BaseStatistics> PropagateExpression(BoundAggregateExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundBetweenExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundCaseExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundCastExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundFunctionExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundComparisonExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundConstantExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundOperatorExpression &expr, unique_ptr<Expression> *expr_ptr);

	void ReplaceWithEmptyResult(unique_ptr<LogicalOperator> &node);

	bool ExpressionIsConstant(Expression &expr, const Value &val);
	bool ExpressionIsConstantOrNull(Expression &expr, const Value &val);

private:
	ClientContext &context;
	//! The map of ColumnBinding -> statistics for the various nodes
	column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map;
	//! Node stats for the current node
	unique_ptr<NodeStatistics> node_stats;
};

} // namespace duckdb
