//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/query_node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

class Deserializer;
class Serializer;

enum class QueryNodeType : uint8_t {
	SELECT_NODE = 1,
	SET_OPERATION_NODE = 2,
	BOUND_SUBQUERY_NODE = 3,
	RECURSIVE_CTE_NODE = 4,
	CTE_NODE = 5,
	STATEMENT_NODE = 6,
	UPDATE_QUERY_NODE = 7,
	DELETE_QUERY_NODE = 8,
	INSERT_QUERY_NODE = 9,
	MERGE_QUERY_NODE = 10,
	COPY_QUERY_NODE = 11
};

struct CommonTableExpressionInfo;

class CommonTableExpressionMap {
public:
	CommonTableExpressionMap();

	InsertionOrderPreservingMap<unique_ptr<CommonTableExpressionInfo>, Identifier, identifier_map_t<idx_t>> map;

public:
	string ToString() const;
	CommonTableExpressionMap Copy() const;

	void Serialize(Serializer &serializer) const;
	static CommonTableExpressionMap Deserialize(Deserializer &deserializer);
};

class QueryNode {
public:
	explicit QueryNode(QueryNodeType type) : type(type) {
	}
	virtual ~QueryNode() {
	}

	//! The type of the query node, either SetOperation or Select
	QueryNodeType type;
	//! The set of result modifiers associated with this query node
	vector<unique_ptr<ResultModifier>> modifiers;
	//! CTEs (used by SelectNode and SetOperationNode)
	CommonTableExpressionMap cte_map;

public:
	//! Convert the query node to a string
	virtual string ToString() const = 0;

	virtual bool Equals(const QueryNode *other) const;

	//! Create a copy of this QueryNode
	virtual unique_ptr<QueryNode> Copy() const = 0;

	string ResultModifiersToString() const;

	//! Adds a distinct modifier to the query node
	void AddDistinct();

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<QueryNode> Deserialize(Deserializer &deserializer);
	//! Throws a SerializationException if a deserialized query node does not have a valid shape
	static void VerifyDeserialized(const unique_ptr<QueryNode> &node);

protected:
	//! Copy base QueryNode properties from another expression to this one,
	//! used in Copy method
	void CopyProperties(QueryNode &other) const;

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast query node to type - query node type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast query node to type - query node type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
