#include "duckdb/planner/operator/logical_external_resource.hpp"

namespace duckdb {

LogicalExternalResource::LogicalExternalResource(BoundExternalResource data_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_EXTERNAL_RESOURCE), data(std::move(data_p)) {
}

idx_t LogicalExternalResource::EstimateCardinality(ClientContext &context) {
	return 1;
}

//! Every form returns (status, name, type, handle): one row for the single forms, one per source row for
//! REGISTER ... FROM <source>.
vector<ColumnBinding> LogicalExternalResource::GetColumnBindings() {
	vector<ColumnBinding> result;
	TableIndex table_index(0);
	for (auto column_index : ProjectionIndex::GetIndexes(4)) {
		result.emplace_back(table_index, column_index);
	}
	return result;
}

void LogicalExternalResource::ResolveTypes() {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)};
}

} // namespace duckdb
