//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_reference_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A BoundReferenceExpression represents a physical index into a DataChunk
class BoundReferenceExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_REF;

public:
	BoundReferenceExpression(Identifier alias, LogicalType type, idx_t index);
	BoundReferenceExpression(LogicalType type, storage_t index);

public:
	idx_t Index() const {
		return index;
	}
	//! Create a reference to a different index
	unique_ptr<Expression> WithIndex(idx_t new_index) const;
	bool IsScalar() const override {
		return false;
	}
	bool IsFoldable() const override {
		return false;
	}

	string ToString() const override;

	hash_t Hash() const override;
	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	//! Index used to access data in the chunks
	storage_t index;
};
} // namespace duckdb
