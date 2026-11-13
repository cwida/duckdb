//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_constant_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BaseStatistics;

class BoundConstantExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_CONSTANT;

public:
	explicit BoundConstantExpression(Value value);
	BoundConstantExpression(Value value, Identifier alias, QueryLocation location = QueryLocation());
	~BoundConstantExpression() override;

public:
	const Value &GetValue() const {
		return value;
	}
	//! Statistics of the value, computed once and shared by every caller
	const BaseStatistics &GetStatistics() const;
	//! Create a constant with a different value (and the type of that value)
	unique_ptr<Expression> WithValue(Value new_value) const;
	void SetReturnType(LogicalType type) override;
	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	Value value;
	mutable mutex statistics_lock;
	mutable unique_ptr<BaseStatistics> statistics;
};
} // namespace duckdb
