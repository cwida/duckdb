#include "duckdb/planner/operator/logical_trampoline.hpp"

#include "duckdb/main/config.hpp"

namespace duckdb {

LogicalTrampoline::LogicalTrampoline() : LogicalCTE(LogicalOperatorType::LOGICAL_TRAMPOLINE), selector_idx(0) {
}

LogicalTrampoline::LogicalTrampoline(Identifier ctename_p, TableIndex table_index_p, idx_t column_count_p,
                                     idx_t selector_idx, unique_ptr<LogicalOperator> seed,
                                     vector<unique_ptr<LogicalOperator>> branches)
    : LogicalCTE(LogicalOperatorType::LOGICAL_TRAMPOLINE), selector_idx(selector_idx) {
	ctename = std::move(ctename_p);
	table_index = table_index_p;
	column_count = column_count_p;
	children.push_back(std::move(seed));
	for (auto &branch : branches) {
		children.push_back(std::move(branch));
	}
}

InsertionOrderPreservingMap<string> LogicalTrampoline::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	result["Branches"] = StringUtil::Format("%llu", BranchCount());
	SetParamsEstimatedCardinality(result);
	return result;
}

vector<TableIndex> LogicalTrampoline::GetTableIndex() const {
	return vector<TableIndex> {table_index};
}

string LogicalTrampoline::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", table_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

void LogicalTrampoline::ResolveTypes() {
	types = children[0]->types;
}

} // namespace duckdb
