//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_subquery_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundSubqueryExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_SUBQUERY;

public:
	explicit BoundSubqueryExpression(LogicalType return_type);

	bool IsCorrelated() const {
		return !binder->correlated_columns.empty();
	}

public:
	const shared_ptr<Binder> &GetBinder() const {
		return binder;
	}
	shared_ptr<Binder> &GetBinderMutable() {
		return binder;
	}
	const BoundStatement &Subquery() const {
		return subquery;
	}
	BoundStatement &SubqueryMutable() {
		return subquery;
	}
	SubqueryType GetSubqueryType() const {
		return subquery_type;
	}
	SubqueryType &SubqueryTypeMutable() {
		return subquery_type;
	}
	const vector<unique_ptr<Expression>> &GetChildren() const {
		return children;
	}
	vector<unique_ptr<Expression>> &GetChildrenMutable() {
		return children;
	}
	const vector<LogicalType> &GetChildTypes() const {
		return child_types;
	}
	vector<LogicalType> &ChildTypesMutable() {
		return child_types;
	}
	const vector<LogicalType> &GetChildTargets() const {
		return child_targets;
	}
	vector<LogicalType> &ChildTargetsMutable() {
		return child_targets;
	}
	ExpressionType ComparisonType() const {
		return comparison_type;
	}
	ExpressionType &ComparisonTypeMutable() {
		return comparison_type;
	}
	bool HasSubquery() const override {
		return true;
	}
	bool IsScalar() const override {
		return false;
	}
	bool IsFoldable() const override {
		return false;
	}
	bool IsVolatile() const override;

	//! Whether errors captured inside this TRY subquery are re-raised at the use site after flattening
	bool RethrowsTryErrors() const {
		return rethrow_try_errors;
	}
	void SetRethrowTryErrors() {
		rethrow_try_errors = true;
	}

	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	bool PropagatesNullValues() const override;

private:
	//! The binder used to bind the subquery node
	shared_ptr<Binder> binder;
	//! The bound subquery node
	BoundStatement subquery;
	//! The subquery type
	SubqueryType subquery_type;
	//! the child expressions to compare with (in case of IN, ANY, ALL operators)
	vector<unique_ptr<Expression>> children;
	//! The comparison type of the child expression with the subquery (in case of ANY, ALL operators)
	ExpressionType comparison_type;
	//! The LogicalTypes of the subquery result. Only used for ANY expressions.
	vector<LogicalType> child_types;
	//! The target LogicalType of the subquery result (i.e. to which type it should be casted, if child_type <>
	//! child_target). Only used for ANY expressions.
	vector<LogicalType> child_targets;
	//! Whether this is a TRY subquery whose captured errors are re-raised at the use site
	bool rethrow_try_errors = false;
};
} // namespace duckdb
