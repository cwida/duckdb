//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_aggregate_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include <memory>

namespace duckdb {

class BoundAggregateExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_AGGREGATE;

public:
	BoundAggregateExpression(BoundAggregateFunction function, vector<unique_ptr<Expression>> children,
	                         unique_ptr<Expression> filter, unique_ptr<FunctionData> bind_info,
	                         AggregateType aggr_type);

public:
	bool IsDistinct() const {
		return aggr_type == AggregateType::DISTINCT;
	}

	const BoundAggregateFunction &Function() const {
		return function;
	}
	BoundAggregateFunction &FunctionMutable() {
		return function;
	}
	const vector<unique_ptr<Expression>> &GetChildren() const {
		return children;
	}
	vector<unique_ptr<Expression>> &GetChildrenMutable() {
		return children;
	}
	const unique_ptr<FunctionData> &BindInfo() const {
		return bind_info;
	}
	unique_ptr<FunctionData> &BindInfoMutable() {
		return bind_info;
	}
	AggregateType GetAggregateType() const {
		return aggr_type;
	}
	AggregateType &GetAggregateTypeMutable() {
		return aggr_type;
	}
	const unique_ptr<Expression> &GetFilter() const {
		return filter;
	}
	unique_ptr<Expression> &GetFilterMutable() {
		return filter;
	}
	const unique_ptr<BoundOrderModifier> &GetOrderBys() const {
		return order_bys;
	}
	unique_ptr<BoundOrderModifier> &GetOrderBysMutable() {
		return order_bys;
	}
	AggregateStateExportMode StateExportMode() const {
		return state_export_mode;
	}
	AggregateStateExportMode &StateExportModeMutable() {
		return state_export_mode;
	}

	bool IsAggregate() const override {
		return true;
	}
	bool IsFoldable() const override {
		return false;
	}
	bool IsVolatile() const override;
	bool CanThrow() const override;
	bool PropagatesNullValues() const override;

	string ToString() const override;

	hash_t Hash() const override;
	bool Equals(const BaseExpression &other) const override;
	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	//! The bound function expression
	BoundAggregateFunction function;
	//! List of arguments to the function
	vector<unique_ptr<Expression>> children;
	//! The bound function data (if any)
	unique_ptr<FunctionData> bind_info;
	//! The aggregate type (distinct or non-distinct)
	AggregateType aggr_type;
	//! Whether or not we are exporting state in this aggregate
	AggregateStateExportMode state_export_mode;

	//! Filter for this aggregate
	unique_ptr<Expression> filter;
	//! The order by expression for this aggregate - if any
	unique_ptr<BoundOrderModifier> order_bys;
};
} // namespace duckdb
