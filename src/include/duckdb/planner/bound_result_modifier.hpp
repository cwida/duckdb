//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/bound_result_modifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/limits.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

//! A ResultModifier
class BoundResultModifier {
public:
	explicit BoundResultModifier(ResultModifierType type);
	virtual ~BoundResultModifier();

	ResultModifierType type;

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

struct BoundOrderByNode {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::ORDER_MODIFIER;

public:
	BoundOrderByNode(OrderType type, OrderByNullType null_order, unique_ptr<Expression> expression);
	BoundOrderByNode(OrderType type, OrderByNullType null_order, unique_ptr<Expression> expression,
	                 unique_ptr<BaseStatistics> stats);

	OrderType type;
	OrderByNullType null_order;
	unique_ptr<Expression> expression;
	unique_ptr<BaseStatistics> stats;

public:
	BoundOrderByNode Copy() const;
	bool Equals(const BoundOrderByNode &other) const;
	string GetOrderModifier() const;
	string ToString() const;

	void Serialize(Serializer &serializer) const;
	static BoundOrderByNode Deserialize(Deserializer &deserializer);
};

enum class LimitNodeType : uint8_t {
	UNSET = 0,
	CONSTANT_VALUE = 1,
	CONSTANT_PERCENTAGE = 2,
	EXPRESSION_VALUE = 3,
	EXPRESSION_PERCENTAGE = 4
};

struct BoundLimitNode {
public:
	BoundLimitNode();
	BoundLimitNode(LimitNodeType type, idx_t constant_integer, double constant_percentage,
	               unique_ptr<Expression> expression);

public:
	static BoundLimitNode ConstantValue(int64_t value);
	static BoundLimitNode ConstantPercentage(double percentage);
	static BoundLimitNode ExpressionValue(unique_ptr<Expression> expression);
	static BoundLimitNode ExpressionPercentage(unique_ptr<Expression> expression);

	LimitNodeType Type() const {
		return type;
	}

	//! Returns the constant value, only valid if Type() == CONSTANT_VALUE
	idx_t GetConstantValue() const;
	//! Returns the constant percentage, only valid if Type() == CONSTANT_PERCENTAGE
	double GetConstantPercentage() const;
	//! Returns the constant percentage, only valid if Type() == EXPRESSION_VALUE
	const Expression &GetValueExpression() const;
	//! Returns the constant percentage, only valid if Type() == EXPRESSION_PERCENTAGE
	const Expression &GetPercentageExpression() const;

	//! Returns a pointer to the expression - should only be used for limit-agnostic optimizations.
	//! Prefer using the methods above in other scenarios.
	unique_ptr<Expression> &GetExpression() {
		return expression;
	}
	const unique_ptr<Expression> &GetExpression() const {
		return expression;
	}

	void Serialize(Serializer &serializer) const;
	static BoundLimitNode Deserialize(Deserializer &deserializer);

private:
	LimitNodeType type = LimitNodeType::UNSET;
	//! Integer value, if value is a constant non-percentage
	idx_t constant_integer = 0;
	//! Percentage value, if value is a constant percentage
	double constant_percentage = -1;
	//! Expression in case node is not constant
	unique_ptr<Expression> expression;

private:
	explicit BoundLimitNode(int64_t constant_value);
	explicit BoundLimitNode(double percentage_value);
	explicit BoundLimitNode(unique_ptr<Expression> expression, bool is_percentage);
};

class BoundLimitModifier : public BoundResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::LIMIT_MODIFIER;

public:
	BoundLimitModifier();

	//! LIMIT
	BoundLimitNode limit_val;
	//! OFFSET
	BoundLimitNode offset_val;
};

class BoundOrderModifier : public BoundResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::ORDER_MODIFIER;

public:
	BoundOrderModifier();

	//! List of order nodes
	vector<BoundOrderByNode> orders;

	unique_ptr<BoundOrderModifier> Copy() const;
	static bool Equals(const BoundOrderModifier &left, const BoundOrderModifier &right);
	static bool Equals(const unique_ptr<BoundOrderModifier> &left, const unique_ptr<BoundOrderModifier> &right);

	void Serialize(Serializer &serializer) const;
	static unique_ptr<BoundOrderModifier> Deserialize(Deserializer &deserializer);

	//! Remove unneeded/duplicate order elements.
	//! Returns true of orders is not empty.
	static bool Simplify(vector<BoundOrderByNode> &orders, const vector<unique_ptr<Expression>> &groups,
	                     optional_ptr<vector<GroupingSet>> grouping_sets);
	bool Simplify(const vector<unique_ptr<Expression>> &groups, optional_ptr<vector<GroupingSet>> grouping_sets);
};

enum class DistinctType : uint8_t { DISTINCT = 0, DISTINCT_ON = 1 };

class BoundDistinctModifier : public BoundResultModifier {
public:
	static constexpr const ResultModifierType TYPE = ResultModifierType::DISTINCT_MODIFIER;

public:
	BoundDistinctModifier();

	//! Whether or not this is a DISTINCT or DISTINCT ON
	DistinctType distinct_type;
	//! list of distinct on targets
	vector<unique_ptr<Expression>> target_distincts;
};

} // namespace duckdb
