#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"

namespace duckdb {

bool CTEFilterDependency::MatchesConsumers(const vector<reference<LogicalCTERef>> &references) const {
	if (references.size() != 2) {
		return false;
	}
	idx_t row_scans = 0;
	idx_t domain_scans = 0;
	for (auto &ref : references) {
		row_scans += ref.get().table_index == row_scan;
		domain_scans += ref.get().table_index == domain_scan;
	}
	return row_scans == 1 && domain_scans == 1;
}

InsertionOrderPreservingMap<string> LogicalMaterializedCTE::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	SetParamsEstimatedCardinality(result);
	return result;
}

vector<TableIndex> LogicalMaterializedCTE::GetTableIndex() const {
	return vector<TableIndex> {table_index};
}

} // namespace duckdb
