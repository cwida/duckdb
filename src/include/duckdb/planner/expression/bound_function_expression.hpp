//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_function_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {
class ScalarFunctionCatalogEntry;

//! Represents a function call that has been bound to a base function
class BoundFunctionExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_FUNCTION;

public:
	BoundFunctionExpression(BoundScalarFunction bound_function, vector<unique_ptr<Expression>> arguments,
	                        unique_ptr<FunctionData> bind_info, bool is_operator = false);

public:
	const BoundScalarFunction &Function() const {
		return function;
	}
	BoundScalarFunction &FunctionMutable() {
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
	bool IsOperator() const {
		return is_operator;
	}
	bool &IsOperatorMutable() {
		return is_operator;
	}
	bool RequiresOrderedExecution() const;

	bool IsVolatile() const override;
	bool IsConsistent() const override;
	bool IsFoldable() const override;
	bool CanThrow() const override;
	bool RequiresRebind() const override;
	string ToString() const override;
	bool PropagatesNullValues() const override;
	hash_t Hash() const override;
	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;
	void Verify() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	static ExpressionType GetFunctionExpressionType(const BoundScalarFunction &bound_function,
	                                                const vector<unique_ptr<Expression>> &arguments,
	                                                optional_ptr<FunctionData> bind_info);

private:
	//! The bound function expression
	BoundScalarFunction function;
	//! List of child-expressions of the function
	vector<unique_ptr<Expression>> children;
	//! The bound function data (if any)
	unique_ptr<FunctionData> bind_info;
	//! Whether or not the function is an operator, only used for rendering
	bool is_operator;
};

} // namespace duckdb
