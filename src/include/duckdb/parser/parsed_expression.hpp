//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/base_expression.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class Deserializer;
class ParsedExpression;
class Serializer;

//! Lightweight const view of a ParsedExpression's direct children.
//! Stores up to INLINE_CAPACITY children on the stack; spills to heap beyond that.
//! Iterating yields const ParsedExpression& — read-only access, works on const expressions.
class ConstChildrenView {
public:
	static constexpr idx_t INLINE_CAPACITY = 8;

	ConstChildrenView() : count(0) {
	}
	ConstChildrenView(const ConstChildrenView &) = delete;
	ConstChildrenView &operator=(const ConstChildrenView &) = delete;
	ConstChildrenView(ConstChildrenView &&other) noexcept : count(other.count), overflow(std::move(other.overflow)) {
		if (count <= INLINE_CAPACITY) {
			for (idx_t i = 0; i < count; i++) {
				inline_storage[i] = other.inline_storage[i];
			}
		}
		other.count = 0;
	}

	void Append(const ParsedExpression &child) {
		if (count < INLINE_CAPACITY) {
			inline_storage[count] = &child;
		} else {
			if (overflow.empty()) {
				overflow.reserve(count + 8);
				for (idx_t i = 0; i < count; i++) {
					overflow.push_back(inline_storage[i]);
				}
			}
			overflow.push_back(&child);
		}
		count++;
	}

	struct iterator {
		const ParsedExpression **ptr;
		const ParsedExpression &operator*() const {
			return **ptr;
		}
		iterator &operator++() {
			++ptr;
			return *this;
		}
		bool operator!=(const iterator &other) const {
			return ptr != other.ptr;
		}
	};

	iterator begin() {
		auto *ptr = count <= INLINE_CAPACITY ? inline_storage : overflow.data();
		return {ptr};
	}
	iterator end() {
		auto *ptr = count <= INLINE_CAPACITY ? inline_storage : overflow.data();
		return {ptr + count};
	}

	idx_t size() const {
		return count;
	}
	bool empty() const {
		return count == 0;
	}

private:
	idx_t count;
	const ParsedExpression *inline_storage[INLINE_CAPACITY];
	vector<const ParsedExpression *> overflow;
};

//! Lightweight mutable view of a ParsedExpression's direct children (as owning-pointer references).
//! Stores up to INLINE_CAPACITY children on the stack; spills to heap beyond that.
//! Iterating yields unique_ptr<ParsedExpression>& — supports both reading and in-place replacement.
class ChildrenView {
public:
	static constexpr idx_t INLINE_CAPACITY = 8;

	ChildrenView() : count(0) {
	}
	ChildrenView(const ChildrenView &) = delete;
	ChildrenView &operator=(const ChildrenView &) = delete;
	ChildrenView(ChildrenView &&other) noexcept : count(other.count), overflow(std::move(other.overflow)) {
		if (count <= INLINE_CAPACITY) {
			for (idx_t i = 0; i < count; i++) {
				inline_storage[i] = other.inline_storage[i];
			}
		}
		other.count = 0;
	}

	void Append(unique_ptr<ParsedExpression> &child) {
		if (count < INLINE_CAPACITY) {
			inline_storage[count] = &child;
		} else {
			if (overflow.empty()) {
				overflow.reserve(count + 8);
				for (idx_t i = 0; i < count; i++) {
					overflow.push_back(inline_storage[i]);
				}
			}
			overflow.push_back(&child);
		}
		count++;
	}

	struct iterator {
		unique_ptr<ParsedExpression> **ptr;
		unique_ptr<ParsedExpression> &operator*() const {
			return **ptr;
		}
		iterator &operator++() {
			++ptr;
			return *this;
		}
		bool operator!=(const iterator &other) const {
			return ptr != other.ptr;
		}
	};

	iterator begin() {
		auto *ptr = count <= INLINE_CAPACITY ? inline_storage : overflow.data();
		return {ptr};
	}
	iterator end() {
		auto *ptr = count <= INLINE_CAPACITY ? inline_storage : overflow.data();
		return {ptr + count};
	}

	idx_t size() const {
		return count;
	}
	bool empty() const {
		return count == 0;
	}

private:
	idx_t count;
	unique_ptr<ParsedExpression> *inline_storage[INLINE_CAPACITY];
	vector<unique_ptr<ParsedExpression> *> overflow;
};

//!  The ParsedExpression class is a base class that can represent any expression
//!  part of a SQL statement.
/*!
 The ParsedExpression class is a base class that can represent any expression
 part of a SQL statement. This is, for example, a column reference in a SELECT
 clause, but also operators, aggregates or filters. The Expression is emitted by the parser and does not contain any
 information about bindings to the catalog or to the types. ParsedExpressions are transformed into regular Expressions
 in the Binder.
 */
class ParsedExpression : public BaseExpression {
public:
	//! Create an Expression
	ParsedExpression(ExpressionType type, ExpressionClass expression_class) : BaseExpression(type, expression_class) {
	}

public:
	bool IsAggregate() const override;
	bool IsWindow() const override;
	bool HasSubquery() const override;
	bool IsScalar() const override;
	bool HasParameter() const override;

	bool Equals(const BaseExpression &other) const override;
	virtual bool Equals(const ParsedExpression &other) const;
	hash_t Hash() const override;

	//! Create a copy of this expression
	virtual unique_ptr<ParsedExpression> Copy() const = 0;

	//! Returns a read-only view over all direct ParsedExpression children of this expression
	ConstChildrenView Children() const;
	//! Returns a mutable view over all direct ParsedExpression children (as unique_ptr references)
	ChildrenView ChildrenMutable();

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);
	//! Throws a SerializationException if a deserialized expression does not have a valid shape
	//! Only this expression is verified - its children are verified when they are deserialized
	void VerifyDeserialized();

	//! Copies the base class properties (type, expression_class, alias, query_location) from other into this
	void CopyBase(const ParsedExpression &other);

	static bool Equals(const unique_ptr<ParsedExpression> &left, const unique_ptr<ParsedExpression> &right);
	static bool ListEquals(const vector<unique_ptr<ParsedExpression>> &left,
	                       const vector<unique_ptr<ParsedExpression>> &right);
};

} // namespace duckdb
