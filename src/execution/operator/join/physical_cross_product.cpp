#include "duckdb/execution/operator/join/physical_cross_product.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"

namespace duckdb {

PhysicalCrossProduct::PhysicalCrossProduct(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                           PhysicalOperator &left, PhysicalOperator &right, idx_t estimated_cardinality)
    : CachingPhysicalOperator(physical_plan, PhysicalOperatorType::CROSS_PRODUCT, std::move(types),
                              estimated_cardinality) {
	children.push_back(left);
	children.push_back(right);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class CrossProductGlobalState : public GlobalSinkState {
public:
	explicit CrossProductGlobalState(ClientContext &context, const PhysicalCrossProduct &op)
	    : rhs_materialized(context, op.children[1].get().GetTypes()) {
		ResetState(context);
	}

private:
	void ResetState(ClientContext &context) {
		rhs_materialized.ResetForReuse();
		rhs_materialized.InitializeAppend(append_state);
		GlobalSinkState::Reset(context);
	}

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &context) override {
		ResetState(context);
	}

	ColumnDataCollection rhs_materialized;
	ColumnDataAppendState append_state;
	mutex rhs_lock;
};

class CrossProductLocalSinkState : public LocalSinkState {
public:
	bool SupportsReuse() const override {
		return true;
	}
};

unique_ptr<GlobalSinkState> PhysicalCrossProduct::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<CrossProductGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalCrossProduct::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<CrossProductLocalSinkState>();
}

SinkResultType PhysicalCrossProduct::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &sink = input.global_state.Cast<CrossProductGlobalState>();
	lock_guard<mutex> client_guard(sink.rhs_lock);
	sink.rhs_materialized.Append(sink.append_state, chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
CrossProductExecutor::CrossProductExecutor(ColumnDataCollection &rhs) : rhs(rhs) {
	rhs.InitializeScanChunk(scan_chunk);
	Reset();
}

void CrossProductExecutor::Reset(const DataChunk &input, DataChunk &output) {
	initialized = true;
	finished = false;
	scan_input_chunk = false;
	rhs.InitializeScan(scan_state);
	position_in_chunk = 0;
	scan_chunk.Reset();
}

void CrossProductExecutor::Reset() {
	initialized = false;
	finished = false;
	scan_input_chunk = false;
	position_in_chunk = 0;
	scan_state = ColumnDataScanState();
	scan_chunk.Reset();
}

bool CrossProductExecutor::NextValue(const DataChunk &input, DataChunk &output) {
	if (!initialized) {
		// not initialized yet: initialize the scan
		Reset(input, output);
	}
	position_in_chunk++;
	idx_t chunk_size = scan_input_chunk ? input.size() : scan_chunk.size();
	if (position_in_chunk < chunk_size) {
		return true;
	}
	// fetch the next chunk
	rhs.Scan(scan_state, scan_chunk);
	position_in_chunk = 0;
	if (scan_chunk.size() == 0) {
		return false;
	}
	// the way the cross product works is that we keep one chunk constantly referenced
	// while iterating over the other chunk one value at a time
	// the second one is the chunk we are "scanning"

	// for the engine, it is better if we emit larger chunks
	// hence the chunk that we keep constantly referenced should be the larger of the two
	scan_input_chunk = input.size() < scan_chunk.size();
	return true;
}

OperatorResultType CrossProductExecutor::Execute(const DataChunk &input, DataChunk &output) {
	if (rhs.Count() == 0) {
		// no RHS: empty result
		return OperatorResultType::FINISHED;
	}
	if (!NextValue(input, output)) {
		// ran out of entries on the RHS
		// reset the RHS and move to the next chunk on the LHS
		initialized = false;
		return OperatorResultType::NEED_MORE_INPUT;
	}

	// set up the constant chunk
	auto &constant_chunk = scan_input_chunk ? scan_chunk : input;
	auto col_count = constant_chunk.ColumnCount();
	auto col_offset = scan_input_chunk ? input.ColumnCount() : 0;
	// SetChildCardinality cannot resize a non-flat vector, and a reused output chunk may still hold a stale
	// DICTIONARY_VECTOR (e.g. a recursive-CTE dict flush Reset cannot flatten), so reset to flat before resizing.
	for (auto &v : output.data) {
		auto vtype = v.GetVectorType();
		if (vtype != VectorType::FLAT_VECTOR && vtype != VectorType::CONSTANT_VECTOR) {
			v.Initialize();
		}
	}
	output.SetChildCardinality(constant_chunk.size());
	for (idx_t i = 0; i < col_count; i++) {
		output.data[col_offset + i].Reference(constant_chunk.data[i]);
	}

	// for the chunk that we are scanning, scan a single value from that chunk
	auto &scan = scan_input_chunk ? input : scan_chunk;
	col_count = scan.ColumnCount();
	col_offset = scan_input_chunk ? 0 : input.ColumnCount();
	for (idx_t i = 0; i < col_count; i++) {
		auto &source = scan.data[i];
		auto &target = output.data[col_offset + i];
		if (DictionaryVector::IsGlobalDictionary(source)) {
			// repeat the row within the global dictionary, which its consumers expect in every chunk
			target.Slice(source, position_in_chunk, position_in_chunk + 1);
			target.Slice(*ConstantVector::ZeroSelectionVector(), output.size());
			continue;
		}
		ConstantVector::Reference(target, count_t(output.size()), source, position_in_chunk, scan.size());
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

class CrossProductOperatorState : public CachingOperatorState {
public:
	explicit CrossProductOperatorState(ColumnDataCollection &rhs) : executor(rhs) {
	}

	CrossProductExecutor executor;

	bool SupportsReuse() const override {
		return true;
	}

	void Reset() override {
		ResetCachingState();
		executor.Reset();
	}
};

unique_ptr<OperatorState> PhysicalCrossProduct::GetOperatorState(ExecutionContext &context) const {
	auto &sink = sink_state->Cast<CrossProductGlobalState>();
	return make_uniq<CrossProductOperatorState>(sink.rhs_materialized);
}

OperatorResultType PhysicalCrossProduct::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                         GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<CrossProductOperatorState>();
	return state.executor.Execute(input, chunk);
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalCrossProduct::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	PhysicalJoin::BuildJoinPipelines(current, meta_pipeline, *this);
}

vector<const_reference<PhysicalOperator>> PhysicalCrossProduct::GetSources() const {
	return children[0].get().GetSources();
}

} // namespace duckdb
