//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parser/expression/cast_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "parser/expression.hpp"

namespace duckdb {
//! Represents a type cast from one type to another type
class CastExpression : public Expression {
public:
	CastExpression(TypeId target, unique_ptr<Expression> child) : Expression(ExpressionType::OPERATOR_CAST, target) {
		assert(child);
		this->child = move(child);
	}

	void ResolveType() override;

	unique_ptr<Expression> Copy() const override;

	size_t ChildCount() const override;
	Expression *GetChild(size_t index) const override;
	void ReplaceChild(std::function<unique_ptr<Expression>(unique_ptr<Expression> expression)> callback,
	                  size_t index) override;

	//! Serializes a CastExpression to a stand-alone binary blob
	void Serialize(Serializer &serializer) override;
	//! Deserializes a blob back into an CastExpression
	static unique_ptr<Expression> Deserialize(ExpressionType type, TypeId return_type, Deserializer &source);

	bool Equals(const Expression *other) const override;

	string ToString() const override {
		return "CAST[" + TypeIdToString(return_type) + "](" + child->ToString() + ")";
	}

	ExpressionClass GetExpressionClass() override {
		return ExpressionClass::CAST;
	}
	//! The child of the cast expression
	unique_ptr<Expression> child;

	//! Add an optional cast to a set of types
	static unique_ptr<Expression> AddCastToType(TypeId type, unique_ptr<Expression> expr) {
		if (expr && expr->GetExpressionClass() == ExpressionClass::PARAMETER) {
			expr->return_type = type;
		}
		if (expr && expr->return_type != type) {
			return make_unique<CastExpression>(type, move(expr));
		}
		return expr;
	}
};
} // namespace duckdb
