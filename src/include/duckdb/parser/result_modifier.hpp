//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/result_modifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
class Deserializer;
class Serializer;

enum class ResultModifierType : uint8_t {
	LIMIT_MODIFIER = 1,
	ORDER_MODIFIER = 2,
	DISTINCT_MODIFIER = 3,
	LEGACY_LIMIT_PERCENT_MODIFIER = 4
};

enum class LimitValueType : uint8_t { ROW_COUNT = 0, PERCENTAGE = 1 };

const char *ToString(ResultModifierType value);
ResultModifierType ResultModifierFromString(const char *value);

//! A ResultModifier
class ResultModifier {
public:
	explicit ResultModifier(ResultModifierType type) : type(type) {
	}
	virtual ~ResultModifier() {
	}

	ResultModifierType type;

public:
	//! Returns true if the two result modifiers are equivalent
	virtual bool Equals(const ResultModifier &other) const;

	//! Create a copy of this ResultModifier
	virtual unique_ptr<ResultModifier> Copy() const = 0;

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<ResultModifier> Deserialize(Deserializer &deserializer);

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast result modifier to type - result modifier type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast result modifier to type - result modifier type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Single node in ORDER BY statement
struct OrderByNode {
	OrderByNode(OrderType type, OrderByNullType null_order, unique_ptr<ParsedExpression> expression)
	    : type(type), null_order(null_order), expression(std::move(expression)) {
	}

	//! Sort order, ASC or DESC
	OrderType type;
	//! The NULL sort order, NULLS_FIRST or NULLS_LAST
	OrderByNullType null_order;
	//! Expression to order by
	unique_ptr<ParsedExpression> expression;

public:
	string ToString() const;

	void Serialize(Serializer &serializer) const;
	static OrderByNode Deserialize(Deserializer &deserializer);
};

class LimitModifier : public ResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::LIMIT_MODIFIER;

public:
	LimitModifier() : ResultModifier(ResultModifierType::LIMIT_MODIFIER) {
	}

	//! LIMIT
	unique_ptr<ParsedExpression> limit;
	//! OFFSET
	unique_ptr<ParsedExpression> offset;
	//! Limit type (row count or percentage)
	LimitValueType limit_type = LimitValueType::ROW_COUNT;

public:
	bool Equals(const ResultModifier &other) const override;
	unique_ptr<ResultModifier> Copy() const override;

	bool UseLegacySerialization() const;
	void LegacySerialize(Serializer &serializer) const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ResultModifier> Deserialize(Deserializer &deserializer);
};

class OrderModifier : public ResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::ORDER_MODIFIER;

public:
	OrderModifier() : ResultModifier(ResultModifierType::ORDER_MODIFIER) {
	}

	//! List of order nodes
	vector<OrderByNode> orders;

public:
	bool Equals(const ResultModifier &other) const override;
	unique_ptr<ResultModifier> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ResultModifier> Deserialize(Deserializer &deserializer);

	static bool Equals(const unique_ptr<OrderModifier> &left, const unique_ptr<OrderModifier> &right);
};

class DistinctModifier : public ResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::DISTINCT_MODIFIER;

public:
	DistinctModifier() : ResultModifier(ResultModifierType::DISTINCT_MODIFIER) {
	}

	//! list of distinct on targets (if any)
	vector<unique_ptr<ParsedExpression>> distinct_on_targets;

public:
	bool Equals(const ResultModifier &other) const override;
	unique_ptr<ResultModifier> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ResultModifier> Deserialize(Deserializer &deserializer);
};

class LegacyLimitPercentModifier : public ResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::LEGACY_LIMIT_PERCENT_MODIFIER;

public:
	LegacyLimitPercentModifier() : ResultModifier(ResultModifierType::LEGACY_LIMIT_PERCENT_MODIFIER) {
	}

	//! LIMIT %
	unique_ptr<ParsedExpression> limit;
	//! OFFSET
	unique_ptr<ParsedExpression> offset;

public:
	bool Equals(const ResultModifier &other) const override;
	unique_ptr<ResultModifier> Copy() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ResultModifier> Deserialize(Deserializer &deserializer);

	static unique_ptr<ResultModifier> DeserializeLegacyLimitPercentModifier(unique_ptr<ParsedExpression> limit,
	                                                                        unique_ptr<ParsedExpression> offset);
};

} // namespace duckdb
