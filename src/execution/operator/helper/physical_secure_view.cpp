#include "duckdb/execution/operator/helper/physical_secure_view.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

PhysicalSecureView::PhysicalSecureView(PhysicalPlan &physical_plan, PhysicalOperator &child, string view_name_p,
                                       vector<string> pushed_filters_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::SECURE_VIEW, child.GetTypes(), estimated_cardinality),
      view_name(std::move(view_name_p)), pushed_filters(std::move(pushed_filters_p)) {
	children.push_back(child);
}

OperatorResultType PhysicalSecureView::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                               GlobalOperatorState &gstate, OperatorState &state) const {
	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
}

InsertionOrderPreservingMap<string> PhysicalSecureView::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["View"] = view_name;
	if (!pushed_filters.empty()) {
		result["Filters"] = StringUtil::Join(pushed_filters, "\n");
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
