//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_window_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/window_range_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

namespace duckdb {

class BoundAggregateFunction;
class BoundWindowFunction;

class BoundWindowExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_WINDOW;

public:
	BoundWindowExpression(LogicalType return_type, unique_ptr<BoundAggregateFunction> aggregate,
	                      unique_ptr<BoundWindowFunction> window, unique_ptr<FunctionData> bind_info);

public:
	const unique_ptr<BoundAggregateFunction> &AggregateFunction() const {
		return aggregate;
	}
	unique_ptr<BoundAggregateFunction> &AggregateFunctionMutable() {
		return aggregate;
	}
	const unique_ptr<BoundWindowFunction> &WindowFunction() const {
		return window;
	}
	unique_ptr<BoundWindowFunction> &WindowFunctionMutable() {
		return window;
	}
	const unique_ptr<FunctionData> &BindInfo() const {
		return bind_info;
	}
	unique_ptr<FunctionData> &BindInfoMutable() {
		return bind_info;
	}
	const vector<unique_ptr<Expression>> &GetChildren() const {
		return children;
	}
	vector<unique_ptr<Expression>> &GetChildrenMutable() {
		return children;
	}
	const vector<unique_ptr<Expression>> &Partitions() const {
		return partitions;
	}
	vector<unique_ptr<Expression>> &PartitionsMutable() {
		return partitions;
	}
	const vector<BoundOrderByNode> &OrderBy() const {
		return orders;
	}
	vector<BoundOrderByNode> &OrderByMutable() {
		return orders;
	}
	const unique_ptr<Expression> &Filter() const {
		return filter_expr;
	}
	unique_ptr<Expression> &FilterMutable() {
		return filter_expr;
	}
	bool IgnoreNulls() const {
		return ignore_nulls;
	}
	bool &IgnoreNullsMutable() {
		return ignore_nulls;
	}
	bool Distinct() const {
		return distinct;
	}
	bool &DistinctMutable() {
		return distinct;
	}
	WindowBoundary WindowStart() const {
		return start;
	}
	WindowBoundary &WindowStartMutable() {
		return start;
	}
	WindowBoundary WindowEnd() const {
		return end;
	}
	WindowBoundary &WindowEndMutable() {
		return end;
	}
	WindowExcludeMode WindowExclude() const {
		return exclude_clause;
	}
	WindowExcludeMode &WindowExcludeMutable() {
		return exclude_clause;
	}
	const unique_ptr<Expression> &StartExpr() const {
		return start_expr;
	}
	unique_ptr<Expression> &StartExprMutable() {
		return start_expr;
	}
	const unique_ptr<Expression> &EndExpr() const {
		return end_expr;
	}
	unique_ptr<Expression> &EndExprMutable() {
		return end_expr;
	}
	void RetainSQLRange(optional_ptr<const Expression> start, optional_ptr<const Expression> end,
	                    const LogicalType &order_type);
	const unique_ptr<ParsedExpression> &SQLRangeStart() const {
		return sql_range_start;
	}
	const unique_ptr<ParsedExpression> &SQLRangeEnd() const {
		return sql_range_end;
	}
	const LogicalType &SQLRangeOrderType() const {
		return sql_range_order_type;
	}
	const unique_ptr<WindowRangeBoundary> &SQLRangeStartBoundary() const {
		return sql_range_start_boundary;
	}
	unique_ptr<WindowRangeBoundary> &SQLRangeStartBoundaryMutable() {
		return sql_range_start_boundary;
	}
	const unique_ptr<WindowRangeBoundary> &SQLRangeEndBoundary() const {
		return sql_range_end_boundary;
	}
	unique_ptr<WindowRangeBoundary> &SQLRangeEndBoundaryMutable() {
		return sql_range_end_boundary;
	}
	const vector<WindowRangeCast> &SQLRangeOrderCasts() const {
		return sql_range_order_casts;
	}
	vector<WindowRangeCast> &SQLRangeOrderCastsMutable() {
		return sql_range_order_casts;
	}
	const vector<BoundOrderByNode> &ArgOrders() const {
		return arg_orders;
	}
	vector<BoundOrderByNode> &ArgOrdersMutable() {
		return arg_orders;
	}
	const vector<unique_ptr<BaseStatistics>> &PartitionsStats() const {
		return partitions_stats;
	}
	vector<unique_ptr<BaseStatistics>> &PartitionsStatsMutable() {
		return partitions_stats;
	}
	const vector<unique_ptr<BaseStatistics>> &ExprStats() const {
		return expr_stats;
	}
	vector<unique_ptr<BaseStatistics>> &ExprStatsMutable() {
		return expr_stats;
	}

	bool IsWindow() const override {
		return true;
	}
	bool IsFoldable() const override {
		return false;
	}
	bool IsVolatile() const override;
	bool CanThrow() const override;

	string ToString() const override;

	//! The number of ordering clauses the functions share
	static idx_t GetSharedOrders(const vector<BoundOrderByNode> &lhs, const vector<BoundOrderByNode> &rhs);
	idx_t GetSharedOrders(const BoundWindowExpression &other) const;

	bool PartitionsAreEquivalent(const BoundWindowExpression &other) const;
	bool KeysAreCompatible(const BoundWindowExpression &other) const;
	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	//! Remove LEAD/LAG offset/default
	vector<unique_ptr<Expression>> SerializedChildren(Serializer &serializer) const;
	unique_ptr<Expression> SerializedOffset(Serializer &serializer) const;
	unique_ptr<Expression> SerializedDefault(Serializer &serializer) const;

private:
	//! The bound aggregate function
	unique_ptr<BoundAggregateFunction> aggregate;
	//! The bound window function
	unique_ptr<BoundWindowFunction> window;
	//! The bound function info
	unique_ptr<FunctionData> bind_info;
	//! The child expressions of the main window function
	vector<unique_ptr<Expression>> children;
	//! The set of expressions to partition by
	vector<unique_ptr<Expression>> partitions;
	//! Statistics belonging to the partitions expressions
	vector<unique_ptr<BaseStatistics>> partitions_stats;
	//! The set of ordering clauses
	vector<BoundOrderByNode> orders;
	//! Expression representing a filter, only used for aggregates
	unique_ptr<Expression> filter_expr;
	//! True to ignore NULL values
	bool ignore_nulls;
	//! Whether or not the aggregate function is distinct, only used for aggregates
	bool distinct;
	//! The window boundaries
	WindowBoundary start = WindowBoundary::INVALID;
	WindowBoundary end = WindowBoundary::INVALID;
	//! The EXCLUDE clause
	WindowExcludeMode exclude_clause = WindowExcludeMode::NO_OTHER;

	unique_ptr<Expression> start_expr;
	unique_ptr<Expression> end_expr;

	//! Literal SQL offsets before endpoint arithmetic; these are not execution children.
	unique_ptr<ParsedExpression> sql_range_start;
	unique_ptr<ParsedExpression> sql_range_end;
	LogicalType sql_range_order_type = LogicalType::INVALID;

	unique_ptr<WindowRangeBoundary> sql_range_start_boundary;
	unique_ptr<WindowRangeBoundary> sql_range_end_boundary;
	vector<WindowRangeCast> sql_range_order_casts;

	//! The set of argument ordering clauses
	//! These are distinct from the frame ordering clauses e.g., the "x" in
	//! FIRST_VALUE(a ORDER BY x) OVER (PARTITION BY p ORDER BY s)
	vector<BoundOrderByNode> arg_orders;

	//! Statistics belonging to the other expressions (start, end, offset, default)
	vector<unique_ptr<BaseStatistics>> expr_stats;
};

} // namespace duckdb
