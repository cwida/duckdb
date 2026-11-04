#include "duckdb/execution/operator/join/physical_positional_join.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"

namespace duckdb {

PhysicalPositionalJoin::PhysicalPositionalJoin(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                               PhysicalOperator &left, PhysicalOperator &right,
                                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::POSITIONAL_JOIN, std::move(types), estimated_cardinality) {
	children.push_back(left);
	children.push_back(right);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class PositionalJoinGlobalState : public GlobalSinkState {
public:
	explicit PositionalJoinGlobalState(ClientContext &context, const PhysicalPositionalJoin &op)
	    : rhs(context, op.children[1].get().GetTypes()), initialized(false), source_count(0), source_offset(0),
	      exhausted(false), rows_scanned(0) {
		rhs.InitializeAppend(append_state);
	}

	ColumnDataCollection rhs;
	ColumnDataAppendState append_state;
	mutex rhs_lock;

	bool initialized;
	ColumnDataScanState scan_state;
	DataChunk source;
	idx_t source_count;
	idx_t source_offset;
	bool exhausted;
	atomic<idx_t> rows_scanned;

	void InitializeScan();
	idx_t Refill();
	idx_t CopyData(DataChunk &output, const idx_t count, const idx_t col_offset);
	void Execute(DataChunk &input, DataChunk &output);
	void GetData(DataChunk &output);
};

unique_ptr<GlobalSinkState> PhysicalPositionalJoin::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<PositionalJoinGlobalState>(context, *this);
}

SinkResultType PhysicalPositionalJoin::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &sink = input.global_state.Cast<PositionalJoinGlobalState>();
	lock_guard<mutex> client_guard(sink.rhs_lock);
	sink.rhs.Append(sink.append_state, chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
void PositionalJoinGlobalState::InitializeScan() {
	if (!initialized) {
		// not initialized yet: initialize the scan
		initialized = true;
		rhs.InitializeScanChunk(source);
		rhs.InitializeScan(scan_state);
	}
}

idx_t PositionalJoinGlobalState::Refill() {
	// Use source_count (not source.size()) to avoid corruption from shared buffers after Reference+SetChildCardinality
	if (source_offset >= source_count) {
		if (!exhausted) {
			source.Reset();
			rhs.Scan(scan_state, source);
			source_count = source.size();
		}
		source_offset = 0;
	}

	const auto available = source_count - source_offset;
	if (!available) {
		if (!exhausted) {
			source.Reset();
			source_count = 0;
			for (idx_t i = 0; i < source.ColumnCount(); ++i) {
				auto &vec = source.data[i];
				ConstantVector::SetNull(vec, count_t(STANDARD_VECTOR_SIZE));
			}
			exhausted = true;
		}
	}

	return available;
}

idx_t PositionalJoinGlobalState::CopyData(DataChunk &output, const idx_t count, const idx_t col_offset) {
	if (!source_offset && (source_count >= count || exhausted)) {
		//	Fast track: aligned and has enough data
		for (idx_t i = 0; i < source.ColumnCount(); ++i) {
			output.data[col_offset + i].Reference(source.data[i]);
		}
		source_offset += count;
	} else {
		// Copy data
		for (idx_t target_offset = 0; target_offset < count;) {
			const auto needed = count - target_offset;
			const auto available = exhausted ? needed : (source_count - source_offset);
			const auto copy_count = MinValue(needed, available);
			const auto source_end = source_offset + copy_count;
			for (idx_t i = 0; i < source.ColumnCount(); ++i) {
				VectorOperations::Copy(source.data[i], output.data[col_offset + i], source_end, source_offset,
				                       target_offset);
			}
			target_offset += copy_count;
			source_offset += copy_count;
			Refill();
		}
	}

	return source.ColumnCount();
}

void PositionalJoinGlobalState::Execute(DataChunk &input, DataChunk &output) {
	lock_guard<mutex> client_guard(rhs_lock);

	// Reference the input and assume it will be full
	const auto col_offset = input.ColumnCount();
	for (idx_t i = 0; i < col_offset; ++i) {
		output.data[i].Reference(input.data[i]);
	}

	// Copy or reference the RHS columns
	const auto count = input.size();
	InitializeScan();
	Refill();
	CopyData(output, count, col_offset);

	output.SetChildCardinality(count);
	const auto scanned = rows_scanned.load(std::memory_order_relaxed);
	rows_scanned.store(scanned + MinValue(count, rhs.Count() - scanned), std::memory_order_relaxed);
}

OperatorResultType PhysicalPositionalJoin::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                   GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &sink = sink_state->Cast<PositionalJoinGlobalState>();
	sink.Execute(input, chunk);
	return OperatorResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
void PositionalJoinGlobalState::GetData(DataChunk &output) {
	lock_guard<mutex> client_guard(rhs_lock);

	InitializeScan();
	Refill();

	//	LHS exhausted
	if (exhausted) {
		//	RHS exhausted too, so we are done
		return;
	}

	//	LHS is all NULL
	const auto col_offset = output.ColumnCount() - source.ColumnCount();
	const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, source_count - source_offset);
	for (idx_t i = 0; i < col_offset; ++i) {
		auto &vec = output.data[i];
		ConstantVector::SetNull(vec, count_t(count));
	}

	//	RHS still has data, so copy it
	CopyData(output, count, col_offset);
	output.SetChildCardinality(count);
	const auto scanned = rows_scanned.load(std::memory_order_relaxed);
	rows_scanned.store(scanned + MinValue(count, rhs.Count() - scanned), std::memory_order_relaxed);
}

SourceResultType PhysicalPositionalJoin::GetDataInternal(ExecutionContext &context, DataChunk &result,
                                                         OperatorSourceInput &input) const {
	auto &sink = sink_state->Cast<PositionalJoinGlobalState>();
	sink.GetData(result);

	return result.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

ProgressData PhysicalPositionalJoin::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &sink = sink_state->Cast<PositionalJoinGlobalState>();
	ProgressData progress;
	progress.total = double(MaxValue<idx_t>(sink.rhs.Count(), 1));
	progress.done = sink.rhs.Count() == 0 ? 1.0 : double(sink.rows_scanned.load(std::memory_order_relaxed));
	return progress;
}

void PhysicalPositionalJoin::SourceFinished(ClientContext &context, GlobalSourceState &gstate) const {
	auto &sink = sink_state->Cast<PositionalJoinGlobalState>();
	sink.rows_scanned.store(sink.rhs.Count(), std::memory_order_relaxed);
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalPositionalJoin::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	PhysicalJoin::BuildJoinPipelines(current, meta_pipeline, *this);
}

vector<const_reference<PhysicalOperator>> PhysicalPositionalJoin::GetSources() const {
	auto result = children[0].get().GetSources();
	if (IsSource()) {
		result.push_back(*this);
	}
	return result;
}

} // namespace duckdb
