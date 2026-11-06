#include "duckdb/planner/operator/logical_external_resource.hpp"

namespace duckdb {

LogicalExternalResource::LogicalExternalResource(BoundExternalResource data_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_EXTERNAL_RESOURCE), data(std::move(data_p)) {
}

idx_t LogicalExternalResource::EstimateCardinality(ClientContext &context) {
	return 1;
}

void LogicalExternalResource::ResolveTypes() {
	types = {LogicalType::BOOLEAN};
}

} // namespace duckdb
