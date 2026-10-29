#include "duckdb/parser/query_node/trampoline_cte_node.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

string TrampolineCTENode::ToString() const {
	string result;
	result = cte_map.ToString();
	result += "(" + seed->ToString() + ")";
	for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
		const auto tee = branch_idx < branch_tee.size() && branch_tee[branch_idx];
		const auto distinct = branch_idx < branch_distinct.size() && branch_distinct[branch_idx];
		const auto merge = branch_idx < branch_merge.size() && branch_merge[branch_idx];
		result += StringUtil::Format(" BRANCH %llu%s%s%s: (", branch_idx + 1, distinct ? " DISTINCT" : "",
		                             merge ? " MERGE" : "", tee ? " TEE" : "");
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
	if (branch_tee != other.branch_tee) {
		return false;
	}
	if (branch_distinct != other.branch_distinct) {
		return false;
	}
	if (branch_merge != other.branch_merge) {
		return false;
	}
	if (emit_distinct != other.emit_distinct) {
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
	result->branch_tee = branch_tee;
	result->branch_distinct = branch_distinct;
	result->branch_merge = branch_merge;
	result->emit_distinct = emit_distinct;
	result->aliases = aliases;
	this->CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
