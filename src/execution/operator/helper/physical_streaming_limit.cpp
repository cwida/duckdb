#include "duckdb/execution/operator/helper/physical_streaming_limit.hpp"
#include "duckdb/execution/operator/helper/physical_limit.hpp"

namespace duckdb {

PhysicalStreamingLimit::PhysicalStreamingLimit(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                               BoundLimitNode limit_val_p, BoundLimitNode offset_val_p,
                                               idx_t estimated_cardinality, bool parallel)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::STREAMING_LIMIT, std::move(types), estimated_cardinality),
      limit_val(std::move(limit_val_p)), offset_val(std::move(offset_val_p)), parallel(parallel) {
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
class StreamingLimitOperatorState : public OperatorState {
public:
	explicit StreamingLimitOperatorState(const PhysicalStreamingLimit &op) {
		PhysicalLimit::SetInitialLimits(op.limit_val, op.offset_val, limit, offset);
	}

	optional_idx limit;
	optional_idx offset;
};

class StreamingLimitGlobalState : public GlobalOperatorState {
public:
	StreamingLimitGlobalState() : current_offset(0) {
	}

	std::atomic<idx_t> current_offset;
};

void PhysicalStreamingLimit::InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
	// every output column only references or slices the input
	chunk.Initialize(allocator, types, vector<bool>(types.size(), false));
}

unique_ptr<OperatorState> PhysicalStreamingLimit::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<StreamingLimitOperatorState>(*this);
}

unique_ptr<GlobalOperatorState> PhysicalStreamingLimit::GetGlobalOperatorState(ClientContext &context) const {
	return make_uniq<StreamingLimitGlobalState>();
}

OperatorResultType PhysicalStreamingLimit::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                   GlobalOperatorState &gstate_p, OperatorState &state_p) const {
	auto &gstate = gstate_p.Cast<StreamingLimitGlobalState>();
	auto &state = state_p.Cast<StreamingLimitOperatorState>();
	auto &limit = state.limit;
	auto &offset = state.offset;
	idx_t current_offset = gstate.current_offset.fetch_add(input.size());
	idx_t max_element;
	if (!PhysicalLimit::ComputeOffset(context, input, limit, offset, current_offset, max_element, limit_val,
	                                  offset_val)) {
		return OperatorResultType::FINISHED;
	}
	if (PhysicalLimit::HandleOffset(input, current_offset, offset.GetIndex(), limit.GetIndex())) {
		chunk.Reference(input);
	}
	if (current_offset >= limit.GetIndex() + offset.GetIndex()) {
		return chunk.size() == 0 ? OperatorResultType::FINISHED : OperatorResultType::HAVE_MORE_OUTPUT;
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

OrderPreservationType PhysicalStreamingLimit::OperatorOrder() const {
	return OrderPreservationType::FIXED_ORDER;
}

bool PhysicalStreamingLimit::ParallelOperator() const {
	return parallel;
}

InsertionOrderPreservingMap<string> PhysicalStreamingLimit::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	if (limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		result["Limit"] = to_string(limit_val.GetConstantValue());
	} else if (limit_val.Type() == LimitNodeType::CONSTANT_PERCENTAGE) {
		result["Limit"] = StringUtil::ToString(limit_val.GetConstantPercentage()) + "%";
	}
	if (offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		auto offset = offset_val.GetConstantValue();
		if (offset > 0) {
			result["Offset"] = to_string(offset);
		}
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
