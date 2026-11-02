//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/base_expression.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
class BaseStatistics;
class ClientContext;

//!  The Expression class represents a bound Expression with a return type
class Expression : public BaseExpression {
public:
	Expression(ExpressionType type, ExpressionClass expression_class, LogicalType return_type);
	~Expression() override;

protected:
	//! The return type of the expression
	LogicalType return_type;

protected:
	//! Expression statistics (if any) - ONLY USED FOR VERIFICATION
	unique_ptr<BaseStatistics> verification_stats;

public:
	const LogicalType &GetReturnType() const {
		return return_type;
	}
	virtual void SetReturnType(LogicalType type) {
		return_type = std::move(type);
	}

	//! Preserve the SQL result type when replacing an expression with an equivalent value.
	static unique_ptr<Expression> PreserveReturnType(const LogicalType &type, unique_ptr<Expression> replacement);

	const unique_ptr<BaseStatistics> &GetVerificationStats() const {
		return verification_stats;
	}
	void SetVerificationStats(unique_ptr<BaseStatistics> stats);

	bool IsAggregate() const override;
	bool IsWindow() const override;
	bool HasSubquery() const override;
	bool IsScalar() const override;
	bool HasParameter() const override;

	virtual bool IsVolatile() const;
	//! Whether the expression returns the same result for the same input within a query
	virtual bool IsConsistent() const;
	virtual bool PropagatesNullValues() const;
	virtual bool IsFoldable() const;
	virtual bool CanThrow() const;
	//! Whether a function or a cast of the expression can return another result in another query, so a statement that
	//! contains it must be bound again
	virtual bool RequiresRebind() const;

	hash_t Hash() const override;

	bool Equals(const BaseExpression &other) const override {
		if (!BaseExpression::Equals(other)) {
			return false;
		}
		return return_type == reinterpret_cast<const Expression &>(other).return_type;
	}
	static bool Equals(const Expression &left, const Expression &right) {
		return left.Equals(right);
	}
	static bool Equals(const unique_ptr<Expression> &left, const unique_ptr<Expression> &right);
	static bool ListEquals(const vector<unique_ptr<Expression>> &left, const vector<unique_ptr<Expression>> &right);
	//! Create a copy of this expression
	virtual unique_ptr<Expression> Copy() const = 0;

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

protected:
	//! Copy base Expression properties from another expression to this one,
	//! used in Copy method
	void CopyProperties(const Expression &other) {
		type = other.type;
		expression_class = other.expression_class;
		alias = other.alias;
		SetReturnType(other.return_type);
		query_location = other.query_location;
	}
};

} // namespace duckdb
