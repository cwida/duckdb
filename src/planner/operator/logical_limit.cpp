#include "duckdb/planner/operator/logical_limit.hpp"

#include "duckdb/logging/logger.hpp"

namespace duckdb {

LogicalLimit::LogicalLimit(BoundLimitNode limit_val, BoundLimitNode offset_val)
    : LogicalOperator(LogicalOperatorType::LOGICAL_LIMIT), limit_val(std::move(limit_val)),
      offset_val(std::move(offset_val)) {
}

vector<ColumnBinding> LogicalLimit::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

idx_t LogicalLimit::EstimateCardinality(ClientContext &context) {
	auto child_cardinality = children[0]->EstimateCardinality(context);
	switch (limit_val.Type()) {
	case LimitNodeType::CONSTANT_VALUE:
		if (limit_val.GetConstantValue() < child_cardinality) {
			child_cardinality = limit_val.GetConstantValue();
		}
		break;
	case LimitNodeType::CONSTANT_PERCENTAGE: {
		// GetConstantPercentage() returns the raw percentage (e.g. 50 for 50%); divide by 100 and clamp to the
		// child cardinality so the idx_t conversion cannot overflow for a large child cardinality
		auto estimate = double(child_cardinality) * limit_val.GetConstantPercentage() / 100.0;
		if (estimate < double(child_cardinality)) {
			child_cardinality = idx_t(estimate);
		}
		break;
	}
	default:
		break;
	}
	return child_cardinality;
}

void LogicalLimit::WarnUnorderedOffset(ClientContext &context) {
	DUCKDB_LOG_WARNING(context,
	                   "OFFSET is applied to a query whose row order is not guaranteed - the result may "
	                   "differ between runs. This happens when the query contains order-breaking operators (e.g. "
	                   "hash joins or aggregations), reads from a source that does not preserve order, or when "
	                   "preserve_insertion_order=false. Add an ORDER BY to make the result deterministic.");
}

void LogicalLimit::ResolveTypes() {
	types = children[0]->types;
}

} // namespace duckdb
