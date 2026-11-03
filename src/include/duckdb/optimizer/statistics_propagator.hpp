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
#include "duckdb/common/table_index.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_tokens.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

namespace duckdb {

class Optimizer;
class ClientContext;
class LogicalOperator;
class TableFilter;
struct BoundOrderByNode;
struct FunctionStatisticsInput;

enum class StatisticsPropagationMode : uint8_t { FILTER_SIMPLIFICATION, FULL };

class StatisticsPropagator {
public:
	StatisticsPropagator(Optimizer &optimizer, unique_ptr<LogicalOperator> &root,
	                     StatisticsPropagationMode mode = StatisticsPropagationMode::FULL);

	unique_ptr<NodeStatistics> PropagateStatistics(unique_ptr<LogicalOperator> &node_ptr);

	column_binding_map_t<unique_ptr<BaseStatistics>> GetStatisticsMap() {
		return std::move(statistics_map);
	}
	bool FilterBindingsChanged() const {
		return filter_bindings_changed;
	}
	bool HasRemovedExpressions() const {
		return removed_expressions;
	}

	//! Evaluate a function when every argument is known to be constant from its statistics.
	static unique_ptr<BaseStatistics> PropagateConstantInputs(ClientContext &context,
	                                                          const BoundFunctionExpression &func,
	                                                          const vector<BaseStatistics> &child_stats);
	//! Derive output statistics of a monotone function by evaluating it at the corners of its
	//! argument ranges (see ArgProperties). Returns nullptr when the bounds cannot be derived.
	static unique_ptr<BaseStatistics> PropagateMonotoneBounds(ClientContext &context,
	                                                          const BoundFunctionExpression &func,
	                                                          const vector<BaseStatistics> &child_stats);
	//! Statistics callback for monotone functions that return NULL for infinite arguments
	DUCKDB_API static unique_ptr<BaseStatistics> PropagateFiniteMonotoneBounds(ClientContext &context,
	                                                                           FunctionStatisticsInput &input);
	//! Compare two sets of statistics and return whether the comparison is always true or false
	static FilterPropagateResult PropagateComparison(const BaseStatistics &left, const BaseStatistics &right,
	                                                 ExpressionType comparison);

private:
	//! Propagate statistics through an operator
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalOperator &node, unique_ptr<LogicalOperator> &node_ptr);

	unique_ptr<NodeStatistics> PropagateStatistics(LogicalCopyToFile &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalCTERef &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalMaterializedCTE &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalFilter &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalGet &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalJoin &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalPositionalJoin &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalProjection &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalSecureView &op, unique_ptr<LogicalOperator> &node_ptr);
	void PropagateStatistics(LogicalComparisonJoin &op, unique_ptr<LogicalOperator> &node_ptr);
	void PropagateStatistics(LogicalAnyJoin &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalSetOperation &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalAggregate &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalCrossProduct &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalLimit &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalOrder &op, unique_ptr<LogicalOperator> &node_ptr);
	unique_ptr<NodeStatistics> PropagateStatistics(LogicalWindow &op, unique_ptr<LogicalOperator> &node_ptr);

	unique_ptr<NodeStatistics> PropagateChildren(LogicalOperator &node, unique_ptr<LogicalOperator> &node_ptr);

	//! Return statistics from a constant value
	unique_ptr<BaseStatistics> StatisticsFromValue(const Value &input);
	//! Update filter statistics from a filter with a constant
	void UpdateFilterStatistics(BaseStatistics &input, ExpressionType comparison_type, const Value &constant);
	//! Update statistics from a filter between two stats
	void UpdateFilterStatistics(BaseStatistics &lstats, BaseStatistics &rstats, ExpressionType comparison_type);
	//! Update filter statistics from a generic comparison
	void UpdateFilterStatistics(const Expression &left, const Expression &right, ExpressionType comparison_type);
	//! Update filter statistics from an expression
	void UpdateFilterStatistics(const Expression &condition);
	//! Set the statistics of a specific column binding to not contain null values
	void SetStatisticsNotNull(ColumnBinding binding);
	//! Reset additive statistics of bindings whose rows an operator can emit more than once
	void ResetAdditiveStatistics(const vector<ColumnBinding> &bindings);
	//! Reset additive statistics in the verification statistics of an expression tree
	void ResetAdditiveVerificationStats(Expression &expr);
	//! Determine whether a propagated condition can be pruned
	FilterPropagateResult ClassifyFilter(Expression &condition);
	//! Simplify conjunctions using filter truth semantics
	bool SimplifyFilter(unique_ptr<Expression> &condition);
	//! Remove constant_or_null(TRUE, ...) conjuncts whose NULL checks are implied by retained predicates
	void SimplifyConstantOrNull(vector<unique_ptr<Expression>> &expressions);
	//! Propagate a filter condition
	FilterPropagateResult HandleFilter(unique_ptr<Expression> &condition);
	//! Rewrite a join whose condition can never match; returns true if the operator was replaced
	bool HandleJoinNeverMatches(LogicalJoin &join, unique_ptr<LogicalOperator> &node_ptr);
	//! Rewrite a join whose condition always matches; returns true if the operator was replaced
	bool HandleJoinAlwaysMatches(LogicalJoin &join, unique_ptr<LogicalOperator> &node_ptr);

	//! Run a comparison between the statistics and the table filter; returns the prune result
	FilterPropagateResult PropagateTableFilter(ColumnBinding stats_binding, BaseStatistics &stats, TableFilter &filter);
	//! Update filter statistics from a TableFilter
	void UpdateFilterStatistics(BaseStatistics &input, const TableFilter &filter);
	//! Update filter statistics from an ExpressionFilter expression
	void UpdateExpressionFilterStatistics(BaseStatistics &input, const Expression &expr);

	//! Add cardinalities together (i.e. new max is stats.max + new_stats.max): used for union
	void AddCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats);
	//! Multiply the cardinalities together (i.e. new max cardinality is stats.max * new_stats.max): used for
	//! joins/cross products
	void MultiplyCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats);
	//! Creates and pushes down a filter based on join statistics
	void CreateFilterFromJoinStats(unique_ptr<LogicalOperator> &child, unique_ptr<Expression> &expr,
	                               const BaseStatistics &stats_before, const BaseStatistics &stats_after);

	unique_ptr<BaseStatistics> PropagateExpression(unique_ptr<Expression> &expr);
	unique_ptr<BaseStatistics> PropagateExpression(Expression &expr, unique_ptr<Expression> &expr_ptr);
	//! Run a comparison between the statistics and the table filter; returns the prune result
	unique_ptr<BaseStatistics> PropagateExpression(BoundAggregateExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateBetween(BoundFunctionExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundCaseExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundConjunctionExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundFunctionExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundConstantExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundColumnRefExpression &expr, unique_ptr<Expression> &expr_ptr);
	unique_ptr<BaseStatistics> PropagateExpression(BoundOperatorExpression &expr, unique_ptr<Expression> &expr_ptr);

	unique_ptr<BaseStatistics> PropagateComparison(BoundFunctionExpression &expr, unique_ptr<Expression> &expr_ptr);
	enum class ComparisonContext { SCALAR, JOIN };
	//! Simplify distinctness when NULL cannot affect the comparison's result in this context
	static ExpressionType SimplifyDistinctComparison(ExpressionType comparison, optional_ptr<const BaseStatistics> left,
	                                                 optional_ptr<const BaseStatistics> right,
	                                                 ComparisonContext context);

	//! Try to execute aggregates using only the statistics if possible
	void TryExecuteAggregates(LogicalAggregate &op, unique_ptr<LogicalOperator> &node_ptr);
	void ReplaceWithEmptyResult(unique_ptr<LogicalOperator> &node);

	bool ExpressionIsConstant(Expression &expr, const Value &val);
	bool ExpressionIsConstantOrNull(Expression &expr, const Value &val);

	unique_ptr<NodeStatistics> PropagateUnion(LogicalSetOperation &setop, unique_ptr<LogicalOperator> &node_ptr);

private:
	Optimizer &optimizer;
	ClientContext &context;
	StatisticsPropagationMode mode;
	//! Reference to the owning pointer so root replacements remain visible
	unique_ptr<LogicalOperator> &root;
	//! The map of ColumnBinding -> statistics for the various nodes
	column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map;
	//! The statistics of a materialized CTE definition, which hold for every reference to it
	struct CTEStatistics {
		//! Statistics of the columns emitted by the definition, by position
		vector<unique_ptr<BaseStatistics>> column_stats;
		//! Cardinality of the definition
		unique_ptr<NodeStatistics> node_stats;
	};
	//! The map of CTE index -> statistics of its definition
	unordered_map<TableIndex, CTEStatistics> cte_stats_map;
	//! Whether any expression, filter or join condition was removed, leaving columns that may now be unused
	bool removed_expressions = false;
	//! Node stats for the current node
	unique_ptr<NodeStatistics> node_stats;
	//! Whether statistics changed which relations a filter depends on
	bool filter_bindings_changed = false;
	//! Whether propagation labels the expressions it visits with the statistics of the current point in the plan
	//! (verification statistics, only used by debug_verify_stats). Operators that re-propagate an expression after
	//! narrowing the statistics of their input rows switch this off while they do, because those narrowed statistics
	//! do not describe the rows the expression is evaluated over.
	bool record_verification_stats = true;
	//! The chain of operators from the root down to the operator currently being propagated
	vector<reference<unique_ptr<LogicalOperator>>> propagation_path;
};

} // namespace duckdb
