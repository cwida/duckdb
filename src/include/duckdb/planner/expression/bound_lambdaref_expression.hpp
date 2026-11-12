//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_lambdaref_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A BoundLambdaRef expression represents a LambdaRef expression that was bound to an lambda parameter
//! in the lambda bindings vector. When capturing lambdas, the BoundLambdaRef becomes a
//! BoundReferenceExpression, indexing the corresponding lambda parameter in the lambda bindings vector,
//! which refers to the physical chunk of the lambda parameter during execution.
class BoundLambdaRefExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_LAMBDA_REF;

public:
	BoundLambdaRefExpression(LogicalType type, ColumnBinding binding, idx_t lambda_idx, idx_t depth = 0);
	BoundLambdaRefExpression(Identifier alias, LogicalType type, ColumnBinding binding, idx_t lambda_idx,
	                         idx_t depth = 0);

public:
	const ColumnBinding &Binding() const {
		return binding;
	}
	idx_t LambdaIndex() const {
		return lambda_idx;
	}
	idx_t Depth() const {
		return depth;
	}
	bool IsScalar() const override {
		return false;
	}
	bool IsFoldable() const override {
		return false;
	}

	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	//! Column index set by the binder, used to generate the final BoundReferenceExpression
	ColumnBinding binding;
	//! The index of the lambda parameter in the lambda bindings vector
	idx_t lambda_idx;
	//! The subquery depth (i.e. depth 0 = current query, depth 1 = parent query, depth 2 = parent of parent, etc...).
	//! This is only non-zero for correlated expressions inside subqueries.
	idx_t depth;
};
} // namespace duckdb
