//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_parameter_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"

namespace duckdb {

class BoundParameterExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_PARAMETER;

public:
	explicit BoundParameterExpression(const duckdb::Identifier &identifier,
	                                  duckdb::Identifier alias = duckdb::Identifier());

public:
	const duckdb::Identifier &Identifier() const {
		return identifier;
	}
	duckdb::Identifier &IdentifierMutable() {
		return identifier;
	}
	const shared_ptr<BoundParameterData> &ParameterData() const {
		return parameter_data;
	}
	shared_ptr<BoundParameterData> &ParameterDataMutable() {
		return parameter_data;
	}

	//! Invalidate a bound parameter expression - forcing a rebind on any subsequent filters
	DUCKDB_API static void Invalidate(Expression &expr);
	//! Invalidate all parameters within an expression
	DUCKDB_API static void InvalidateRecursive(Expression &expr);

	bool IsScalar() const override;
	bool HasParameter() const override;
	bool IsFoldable() const override;

	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	BoundParameterExpression(bound_parameter_map_t &global_parameter_set, duckdb::Identifier identifier,
	                         LogicalType return_type, shared_ptr<BoundParameterData> parameter_data);

private:
	duckdb::Identifier identifier;
	shared_ptr<BoundParameterData> parameter_data;
};

} // namespace duckdb
