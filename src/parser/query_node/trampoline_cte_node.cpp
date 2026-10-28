#include "duckdb/parser/query_node/trampoline_cte_node.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

string TrampolineCTENode::ToString() const {
	string result;
	result = cte_map.ToString();
	result += "(" + seed->ToString() + ")";
	for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
		result += StringUtil::Format(" BRANCH %llu: (", branch_idx + 1);
		result += branches[branch_idx]->ToString();
		result += ")";
	}
	return result;
}

bool TrampolineCTENode::Equals(const QueryNode *other_p) const {
	if (!QueryNode::Equals(other_p)) {
		return false;
	}
	if (this == other_p) {
		return true;
	}
	auto &other = other_p->Cast<TrampolineCTENode>();
	if (selector != other.selector) {
		return false;
	}
	if (!seed->Equals(other.seed.get())) {
		return false;
	}
	if (branches.size() != other.branches.size()) {
		return false;
	}
	for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
		if (!branches[branch_idx]->Equals(other.branches[branch_idx].get())) {
			return false;
		}
	}
	return true;
}

unique_ptr<QueryNode> TrampolineCTENode::Copy() const {
	auto result = make_uniq<TrampolineCTENode>();
	result->ctename = ctename;
	result->selector = selector;
	result->seed = seed->Copy();
	for (auto &branch : branches) {
		result->branches.push_back(branch->Copy());
	}
	result->aliases = aliases;
	this->CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
