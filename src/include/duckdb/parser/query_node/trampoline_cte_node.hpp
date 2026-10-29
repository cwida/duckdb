//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/query_node/trampoline_cte_node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"

namespace duckdb {

//! A trampoline-style CTE: a seed query plus n branch queries. Rows carry a
//! selector column; selector 0 emits a row, selector t routes it to branch t.
class TrampolineCTENode : public QueryNode {
public:
	static constexpr const QueryNodeType TYPE = QueryNodeType::TRAMPOLINE_CTE_NODE;

public:
	TrampolineCTENode() : QueryNode(QueryNodeType::TRAMPOLINE_CTE_NODE) {
	}

	Identifier ctename;
	//! Name of the selector column that routes rows between branches
	Identifier selector;
	//! The seed query producing the initial rows
	unique_ptr<QueryNode> seed;
	//! Branch queries; branches[i] is BRANCH i+1
	vector<unique_ptr<QueryNode>> branches;
	//! Per-branch TEE flag: every row routed to the branch is also emitted
	vector<bool> branch_tee;
	//! Aliases of the trampoline CTE node
	vector<Identifier> aliases;

public:
	string ToString() const override;

	bool Equals(const QueryNode *other) const override;
	unique_ptr<QueryNode> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<QueryNode> Deserialize(Deserializer &source);
};

} // namespace duckdb
