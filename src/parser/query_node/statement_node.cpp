#include "duckdb/parser/query_node/statement_node.hpp"

namespace duckdb {

StatementNode::StatementNode(SQLStatement &stmt_p) : QueryNode(QueryNodeType::STATEMENT_NODE), stmt(stmt_p) {
}

StatementNode::StatementNode(unique_ptr<SQLStatement> stmt_p)
    : QueryNode(QueryNodeType::STATEMENT_NODE), stmt(*stmt_p), owned_statement(std::move(stmt_p)) {
}

//! Convert the query node to a string
string StatementNode::ToString() const {
	return stmt.ToString();
}

bool StatementNode::Equals(const QueryNode *other_p) const {
	if (!QueryNode::Equals(other_p)) {
		return false;
	}
	if (this == other_p) {
		return true;
	}
	auto &other = other_p->Cast<StatementNode>();
	return RefersToSameObject(stmt, other.stmt);
}

//! Create a copy of this SelectNode
unique_ptr<QueryNode> StatementNode::Copy() const {
	if (owned_statement) {
		return make_uniq<StatementNode>(owned_statement->Copy());
	}
	return make_uniq<StatementNode>(stmt);
}

//! Serializes a QueryNode to a stand-alone binary blob
//! Deserializes a blob back into a QueryNode

void StatementNode::Serialize(Serializer &serializer) const {
	throw InternalException("StatementNode cannot be serialized");
}

unique_ptr<QueryNode> StatementNode::Deserialize(Deserializer &source) {
	throw InternalException("StatementNode cannot be deserialized");
}

} // namespace duckdb
