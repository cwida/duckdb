//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_default_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundDefaultExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_DEFAULT;

public:
	explicit BoundDefaultExpression(LogicalType type = LogicalType(), Identifier alias_p = Identifier(),
	                                QueryLocation location = QueryLocation())
	    : Expression(ExpressionType::VALUE_DEFAULT, ExpressionClass::BOUND_DEFAULT, std::move(type)) {
		alias = std::move(alias_p);
		query_location = location;
	}

public:
	bool IsScalar() const override {
		return false;
	}
	bool IsFoldable() const override {
		return false;
	}

	string ToString() const override {
		return "DEFAULT";
	}

	unique_ptr<Expression> Copy() const override {
		return make_uniq<BoundDefaultExpression>(return_type);
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);
};
} // namespace duckdb
