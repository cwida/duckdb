//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/base_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/query_location.hpp"

namespace duckdb {

//!  The BaseExpression class is a base class that can represent any expression
//!  part of a SQL statement.
class BaseExpression {
public:
	//! Create an Expression
	BaseExpression(ExpressionType type, ExpressionClass expression_class)
	    : type(type), expression_class(expression_class) {
	}
	virtual ~BaseExpression() {
	}

	//! Returns the class of the expression
	ExpressionClass GetExpressionClass() const {
		return expression_class;
	}

	//! Returns the type of the expression
	ExpressionType GetExpressionType() const {
		return type;
	}

	//! Sets the type of the expression unsafely. In general expressions are immutable and should not be changed after
	//! creation. Only use this if you know what you are doing.
	void SetExpressionTypeUnsafe(ExpressionType new_type) {
		type = new_type;
	}

	//! Returns the source location in the query (if any). Implicitly converts to optional_idx (start offset).
	QueryLocation GetQueryLocation() const {
		return query_location;
	}

	//! Sets the location in the query
	void SetQueryLocation(optional_idx location) {
		query_location = location;
	}

	//! Sets the source location in the query
	void SetQueryLocation(QueryLocation location) {
		query_location = location;
	}

	//! Returns true if the expression has a valid query location
	bool HasQueryLocation() const {
		return query_location.IsValid();
	}

	//! Returns true if the expression has a non-empty alias
	bool HasAlias() const {
		return !alias.empty();
	}

	//! Returns the alias of the expression
	const Identifier &GetAlias() const {
		return alias;
	}

	//! Sets the alias of the expression
	void SetAlias(Identifier alias_p) {
		alias = std::move(alias_p);
	}

	//! Clears the alias of the expression
	void ClearAlias() {
		alias.clear();
	}

protected:
	//! Type of the expression
	ExpressionType type;

	//! The expression class of the node
	ExpressionClass expression_class;

	//! The alias of the expression,
	Identifier alias;

	//! The source location in the query (if any)
	QueryLocation query_location;

protected:
	//! Sets the class of the expression unsafely. In general expressions are immutable and should not be changed after
	//! creation. Only use this if you know what you are doing.
	void SetExpressionClassUnsafe(ExpressionClass new_class) {
		expression_class = new_class;
	}

public:
	//! Returns true if this expression is an aggregate or not.
	/*!
	 Examples:

	 (1) SUM(a) + 1 -- True

	 (2) a + 1 -- False
	 */
	virtual bool IsAggregate() const = 0;
	//! Returns true if the expression has a window function or not
	virtual bool IsWindow() const = 0;
	//! Returns true if the query contains a subquery
	virtual bool HasSubquery() const = 0;
	//! Returns true if expression does not contain a group ref or col ref or parameter
	virtual bool IsScalar() const = 0;
	//! Returns true if the expression has a parameter
	virtual bool HasParameter() const = 0;

	//! Get the name of the expression
	virtual Identifier GetName() const;
	//! Convert the Expression to a String
	virtual string ToString() const = 0;
	//! Print the expression to stdout
	void Print() const;

	//! Creates a hash value of this expression. It is important that if two expressions are identical (i.e.
	//! Expression::Equals() returns true), that their hash value is identical as well.
	virtual hash_t Hash() const = 0;
	//! Returns true if this expression is equal to another expression
	virtual bool Equals(const BaseExpression &other) const;

	static bool Equals(const BaseExpression &left, const BaseExpression &right) {
		return left.Equals(right);
	}
	bool operator==(const BaseExpression &rhs) const {
		return Equals(rhs);
	}

	virtual void Verify() const;

public:
	template <class TARGET>
	TARGET &Cast() {
		if (expression_class != TARGET::TYPE) {
			InternalException::Throw("Failed to cast expression to type - expression type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (expression_class != TARGET::TYPE) {
			InternalException::Throw("Failed to cast expression to type - expression type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
