#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

string RecursiveCTENode::ToString() const {
	string result;
	result = cte_map.ToString();
	result += "(" + left->ToString() + ")";
	result += " UNION ";
	if (union_all) {
		result += "ALL ";
	}
	result += "(" + right->ToString() + ")";
	return result;
}

bool RecursiveCTENode::Equals(const QueryNode *other_p) const {
	if (!QueryNode::Equals(other_p)) {
		return false;
	}
	if (this == other_p) {
		return true;
	}
	auto &other = other_p->Cast<RecursiveCTENode>();

	if (other.union_all != union_all) {
		return false;
	}

	if (!ParsedExpression::ListEquals(key_targets, other.key_targets)) {
		return false;
	}

	if (!left->Equals(other.left.get())) {
		return false;
	}
	if (!right->Equals(other.right.get())) {
		return false;
	}
	return true;
}

unique_ptr<QueryNode> RecursiveCTENode::Copy() const {
	auto result = make_uniq<RecursiveCTENode>();
	result->ctename = ctename;
	result->union_all = union_all;
	result->left = left->Copy();
	result->right = right->Copy();
	result->aliases = aliases;

	for (auto &key : key_targets) {
		result->key_targets.push_back(key->Copy());
	}

	this->CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
