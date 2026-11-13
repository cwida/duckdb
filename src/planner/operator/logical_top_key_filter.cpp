#include "duckdb/planner/operator/logical_top_key_filter.hpp"

namespace duckdb {

LogicalTopKeyFilter::LogicalTopKeyFilter(vector<BoundOrderByNode> orders, idx_t limit)
    : LogicalOperator(LogicalOperatorType::LOGICAL_TOP_KEY_FILTER), orders(std::move(orders)), limit(limit) {
}

LogicalTopKeyFilter::~LogicalTopKeyFilter() {
}

InsertionOrderPreservingMap<string> LogicalTopKeyFilter::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string keys;
	for (idx_t i = 0; i < orders.size(); i++) {
		if (i > 0) {
			keys += "\n";
		}
		keys += orders[i].ToString();
	}
	result["Keys"] = keys;
	result["Limit"] = to_string(limit);
	SetParamsEstimatedCardinality(result);
	return result;
}

} // namespace duckdb
