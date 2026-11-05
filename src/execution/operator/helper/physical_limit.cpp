#include "duckdb/execution/operator/helper/physical_limit.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/batched_data_collection.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/helper/physical_streaming_limit.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

constexpr const idx_t PhysicalLimit::MAX_LIMIT_VALUE;

PhysicalLimit::PhysicalLimit(PhysicalPlan &physical_plan, vector<LogicalType> types, BoundLimitNode limit_val_p,
                             BoundLimitNode offset_val_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::LIMIT, std::move(types), estimated_cardinality),
      limit_val(std::move(limit_val_p)), offset_val(std::move(offset_val_p)) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class LimitGlobalState : public GlobalSinkState {
public:
	explicit LimitGlobalState(ClientContext &context, const PhysicalLimit &op)
	    : data(context, op.types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR) {
		limit = 0;
		offset = 0;
	}

	mutex glock;
	idx_t limit;
	idx_t offset;
	BatchedDataCollection data;
};

class LimitLocalState : public LocalSinkState {
public:
	explicit LimitLocalState(ClientContext &context, const PhysicalLimit &op)
	    : current_offset(0), data(context, op.types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR) {
		PhysicalLimit::SetInitialLimits(op.limit_val, op.offset_val, limit, offset);
	}

	idx_t current_offset;
	optional_idx limit;
	optional_idx offset;
	BatchedDataCollection data;
};

unique_ptr<GlobalSinkState> PhysicalLimit::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<LimitGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalLimit::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<LimitLocalState>(context.client, *this);
}

void PhysicalLimit::SetInitialLimits(const BoundLimitNode &limit_val, const BoundLimitNode &offset_val,
                                     optional_idx &limit, optional_idx &offset) {
	switch (limit_val.Type()) {
	case LimitNodeType::CONSTANT_VALUE:
		limit = limit_val.GetConstantValue();
		break;
	case LimitNodeType::UNSET:
		limit = MAX_LIMIT_VALUE;
		break;
	default:
		break;
	}
	switch (offset_val.Type()) {
	case LimitNodeType::CONSTANT_VALUE:
		offset = offset_val.GetConstantValue();
		break;
	case LimitNodeType::UNSET:
		offset = 0;
		break;
	default:
		break;
	}
}

idx_t PhysicalLimit::GetLimitValue(const Value &val, idx_t null_value) {
	if (val.IsNull()) {
		return null_value;
	}
	auto value = val.GetValue<idx_t>();
	if (value > MAX_LIMIT_VALUE) {
		throw BinderException("Max value %llu for LIMIT/OFFSET is %llu", value, MAX_LIMIT_VALUE);
	}
	return value;
}

bool PhysicalLimit::ComputeOffset(ExecutionContext &context, DataChunk &input, optional_idx &limit,
                                  optional_idx &offset, idx_t current_offset, idx_t &max_element,
                                  const BoundLimitNode &limit_val, const BoundLimitNode &offset_val) {
	if (!limit.IsValid()) {
		Value val = GetDelimiter(context, input, limit_val.GetValueExpression());
		limit = GetLimitValue(val, MAX_LIMIT_VALUE);
	}
	if (!offset.IsValid()) {
		Value val = GetDelimiter(context, input, offset_val.GetValueExpression());
		offset = GetLimitValue(val, 0);
	}
	max_element = limit.GetIndex() + offset.GetIndex();
	if (limit == 0 || current_offset >= max_element) {
		return false;
	}
	return true;
}

SinkResultType PhysicalLimit::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	D_ASSERT(chunk.size() > 0);
	auto &state = input.local_state.Cast<LimitLocalState>();
	auto &limit = state.limit;
	auto &offset = state.offset;

	idx_t max_element;
	if (!ComputeOffset(context, chunk, limit, offset, state.current_offset, max_element, limit_val, offset_val)) {
		return SinkResultType::FINISHED;
	}
	auto max_cardinality = max_element - state.current_offset;
	if (max_cardinality < chunk.size()) {
		// truncate the chunk to the first max_cardinality rows
		chunk.Slice(0, max_cardinality);
	}
	state.data.Append(chunk, state.partition_info.batch_index.GetIndex());
	state.current_offset += chunk.size();
	if (state.current_offset == max_element) {
		return SinkResultType::FINISHED;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalLimit::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<LimitGlobalState>();
	auto &state = input.local_state.Cast<LimitLocalState>();

	lock_guard<mutex> lock(gstate.glock);
	if (state.limit.IsValid()) {
		gstate.limit = state.limit.GetIndex();
	}
	if (state.offset.IsValid()) {
		gstate.offset = state.offset.GetIndex();
	}
	gstate.data.Merge(state.data);

	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class LimitSourceState : public GlobalSourceState {
public:
	LimitSourceState() {
		initialized = false;
		current_offset = 0;
		scanned_rows = 0;
		total_rows = 0;
	}

	bool initialized;
	idx_t current_offset;
	BatchedChunkScanState scan_state;
	//! The number of rows scanned from the collection (including rows skipped by the offset)
	atomic<idx_t> scanned_rows;
	//! The number of rows that will be scanned - set when the scan is initialized
	atomic<idx_t> total_rows;
};

unique_ptr<GlobalSourceState> PhysicalLimit::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<LimitSourceState>();
}

SourceResultType PhysicalLimit::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<LimitGlobalState>();
	auto &state = input.global_state.Cast<LimitSourceState>();
	while (state.current_offset < gstate.limit + gstate.offset) {
		if (!state.initialized) {
			gstate.data.InitializeScan(state.scan_state);
			idx_t total_rows = gstate.data.Count();
			if (gstate.limit != DConstants::INVALID_INDEX) {
				total_rows = MinValue<idx_t>(total_rows, gstate.limit + gstate.offset);
			}
			state.total_rows = total_rows;
			state.initialized = true;
		}
		gstate.data.Scan(state.scan_state, chunk);
		state.scanned_rows.fetch_add(chunk.size(), std::memory_order_relaxed);
		if (chunk.size() == 0) {
			return SourceResultType::FINISHED;
		}
		if (HandleOffset(chunk, state.current_offset, gstate.offset, gstate.limit)) {
			break;
		}
	}

	return chunk.size() > 0 ? SourceResultType::HAVE_MORE_OUTPUT : SourceResultType::FINISHED;
}

ProgressData PhysicalLimit::GetProgress(ClientContext &context, GlobalSourceState &gstate_p) const {
	auto &state = gstate_p.Cast<LimitSourceState>();
	idx_t total = state.total_rows;
	ProgressData progress;
	progress.total = static_cast<double>(MaxValue<idx_t>(total, 1));
	progress.done = static_cast<double>(MinValue<idx_t>(state.scanned_rows, total));
	return progress;
}

bool PhysicalLimit::HandleOffset(DataChunk &input, idx_t &current_offset, idx_t offset, idx_t limit) {
	idx_t max_element = limit + offset;
	if (limit == DConstants::INVALID_INDEX) {
		max_element = DConstants::INVALID_INDEX;
	}
	idx_t input_size = input.size();
	if (current_offset < offset) {
		// we are not yet at the offset point
		if (current_offset + input.size() > offset) {
			// however we will reach it in this chunk
			// we have to copy part of the chunk with an offset
			idx_t start_position = offset - current_offset;
			auto chunk_count = MinValue<idx_t>(limit, input.size() - start_position);
			SelectionVector sel(STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < chunk_count; i++) {
				sel.set_index(i, start_position + i);
			}
			// set up a slice of the input chunks
			input.Slice(input, sel, chunk_count);
		} else {
			current_offset += input_size;
			return false;
		}
	} else {
		// have to copy either the entire chunk or part of it
		if (current_offset + input.size() >= max_element) {
			// have to limit the count of the chunk
			input.Slice(0, max_element - current_offset);
		}
	}

	current_offset += input_size;
	return true;
}

Value PhysicalLimit::GetDelimiter(ExecutionContext &context, DataChunk &input, const Expression &expr) {
	DataChunk limit_chunk;
	vector<LogicalType> types {expr.GetReturnType()};
	auto &allocator = Allocator::Get(context.client);
	limit_chunk.Initialize(allocator, types);
	ExpressionExecutor limit_executor(context.client, &expr);
	// only evaluate the expression on the first row of the input
	DataChunk single_row_input;
	single_row_input.InitializeEmpty(input.GetTypes());
	single_row_input.Slice(input, 0, MinValue<idx_t>(input.size(), 1));
	limit_executor.Execute(single_row_input, limit_chunk);
	auto limit_value = limit_chunk.GetValue(0, 0);
	return limit_value;
}

InsertionOrderPreservingMap<string> PhysicalLimit::ParamsToString() const {
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
