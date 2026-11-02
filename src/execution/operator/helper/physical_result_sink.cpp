#include "duckdb/execution/operator/helper/physical_result_sink.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/thread_annotation.hpp"
#include "duckdb/main/buffered_data/batched_buffered_data.hpp"
#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_unit.hpp"
#include "duckdb/main/retained_result_collection.hpp"

namespace duckdb {

PhysicalResultSink::PhysicalResultSink(PhysicalPlan &physical_plan, PreparedStatementData &data,
                                       ResultOrdering ordering)
    : PhysicalResultCollector(physical_plan, data), ordering(ordering) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class ResultSinkGlobalState : public GlobalSinkState {
public:
	//! This is weak to avoid creating a cyclical reference
	weak_ptr<ClientContext> context;
	//! The buffer behind the result. It holds the retention decision and the format
	shared_ptr<BufferedData> buffered_data;
	annotated_mutex glock;
	//! The merged retained deposit, once at least one producer has combined into it
	unique_ptr<RetainedResultCollection> collection DUCKDB_GUARDED_BY(glock);
};

class ResultSinkLocalState : public LocalSinkState {
public:
	//! Set once the chunk is appended, so a re-invocation after BLOCKED resumes the drain, not the append
	bool chunk_appended = false;
	//! The batch this producer is currently sinking
	idx_t current_batch = 0;
	//! Created at the first Append, because the lifetime is not settled yet when the local sink state is
	unique_ptr<ResultFormatLocalState> format_state;
	//! The producer's own retained deposit, created lazily at the first retained append and merged
	//! into the global instance in Combine
	unique_ptr<RetainedResultCollection> collection;
};

void PhysicalResultSink::SetResultBuffer(shared_ptr<BufferedData> buffer) {
	D_ASSERT(!result_buffer);
	result_buffer = std::move(buffer);
}

unique_ptr<GlobalSinkState> PhysicalResultSink::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<ResultSinkGlobalState>();
	state->context = context.shared_from_this();
	D_ASSERT(result_buffer);
	state->buffered_data = result_buffer;
	return std::move(state);
}

unique_ptr<LocalSinkState> PhysicalResultSink::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<ResultSinkLocalState>();
}

ResultLifetime PhysicalResultSink::CurrentLifetime(ResultSinkGlobalState &gstate) const {
	return gstate.buffered_data->Lifetime();
}

bool PhysicalResultSink::DrainsByBatchIndex(ResultSinkGlobalState &gstate) const {
	return BatchOrdered() && CurrentLifetime(gstate) != ResultLifetime::RETAINED;
}

ResultFormatLocalState &PhysicalResultSink::LocalFormatState(ResultSinkGlobalState &gstate,
                                                             ResultSinkLocalState &lstate) const {
	if (!lstate.format_state) {
		auto &format = gstate.buffered_data->Format();
		lstate.format_state = format.InitLocal(gstate.buffered_data->FormatState());
	}
	return *lstate.format_state;
}

SinkResultType PhysicalResultSink::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<ResultSinkGlobalState>();
	auto &lstate = input.local_state.Cast<ResultSinkLocalState>();
	auto current = CurrentLifetime(gstate);
	if (current == ResultLifetime::UNDECIDED) {
		// The first chunk waits unconsumed for the consumer's choice; the pipeline re-delivers it on resume
		if (gstate.buffered_data->ParkUndecided(input.interrupt_state)) {
			return SinkResultType::BLOCKED;
		}
		current = gstate.buffered_data->Lifetime();
	}
	if (current == ResultLifetime::RETAINED) {
		return SinkRetained(context, gstate, lstate, chunk);
	}
	return SinkDraining(gstate, lstate, chunk, input);
}

SinkResultType PhysicalResultSink::SinkRetained(ExecutionContext &context, ResultSinkGlobalState &gstate,
                                                ResultSinkLocalState &lstate, DataChunk &chunk) const {
	auto &buffered_data = *gstate.buffered_data;
	if (!lstate.collection) {
		lstate.collection = buffered_data.Format().CreateCollection(context.client, buffered_data.FormatState(),
		                                                            buffered_data.FormatContext());
	}
	// batch_index is unset (throws on GetIndex) for a plan that is not batch ordered
	auto batch = BatchOrdered() ? lstate.partition_info.batch_index.GetIndex() : 0;
	lstate.collection->Append(chunk, batch);
	return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalResultSink::AppendChunk(ResultSinkGlobalState &gstate, ResultSinkLocalState &lstate,
                                     DataChunk &chunk) const {
	auto &format = gstate.buffered_data->Format();
	format.AppendToUnit(gstate.buffered_data->FormatState(), LocalFormatState(gstate, lstate), chunk);
}

bool PhysicalResultSink::DrainFinishedUnits(ResultSinkGlobalState &gstate, ResultSinkLocalState &lstate,
                                            const InterruptState &interrupt) const {
	if (!lstate.format_state) {
		return false;
	}
	auto &format = gstate.buffered_data->Format();
	// A re-invocation after BLOCKED continues here: the delivered unit is already out of the format's state
	while (format.IsUnitFinished(*lstate.format_state)) {
		auto unit = format.FinishUnit(gstate.buffered_data->FormatState(), *lstate.format_state);
		if (HandOver(gstate, lstate, std::move(unit), interrupt)) {
			return true;
		}
	}
	return false;
}

bool PhysicalResultSink::FlushUnits(ResultSinkGlobalState &gstate, ResultSinkLocalState &lstate,
                                    const InterruptState &interrupt) const {
	if (!lstate.format_state) {
		return false;
	}
	auto &format = gstate.buffered_data->Format();
	while (auto unit = format.FinishUnit(gstate.buffered_data->FormatState(), *lstate.format_state)) {
		if (HandOver(gstate, lstate, std::move(unit), interrupt)) {
			return true;
		}
	}
	return false;
}

bool PhysicalResultSink::HandOver(ResultSinkGlobalState &gstate, ResultSinkLocalState &lstate,
                                  unique_ptr<ResultUnit> unit, const InterruptState &interrupt) const {
	D_ASSERT(unit->row_count > 0);
	if (BatchOrdered()) {
		return gstate.buffered_data->Cast<BatchedBufferedData>().AppendOrBlock(std::move(unit), lstate.current_batch,
		                                                                       interrupt);
	}
	return gstate.buffered_data->Cast<SimpleBufferedData>().AppendOrBlock(std::move(unit), interrupt);
}

SinkResultType PhysicalResultSink::SinkDraining(ResultSinkGlobalState &gstate, ResultSinkLocalState &lstate,
                                                DataChunk &chunk, OperatorSinkInput &input) const {
	if (lstate.chunk_appended) {
		// The chunk was appended before the park; the units it still owes come out of the drain below
		lstate.chunk_appended = false;
	} else {
		if (BatchOrdered()) {
			lstate.current_batch = lstate.partition_info.batch_index.GetIndex();
			gstate.buffered_data->Cast<BatchedBufferedData>().UpdateMinBatchIndex(
			    lstate.partition_info.min_batch_index.GetIndex());
		}
		AppendChunk(gstate, lstate, chunk);
	}
	if (DrainFinishedUnits(gstate, lstate, input.interrupt_state)) {
		lstate.chunk_appended = true;
		return SinkResultType::BLOCKED;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalResultSink::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<ResultSinkGlobalState>();
	auto &lstate = input.local_state.Cast<ResultSinkLocalState>();
	if (FlushUnits(gstate, lstate, input.interrupt_state)) {
		return SinkCombineResultType::BLOCKED;
	}
	if (CurrentLifetime(gstate) == ResultLifetime::RETAINED) {
		return CombineRetained(context.client, gstate, lstate);
	}
	return CombineDraining(gstate, lstate);
}

SinkCombineResultType PhysicalResultSink::CombineDraining(ResultSinkGlobalState &gstate,
                                                          ResultSinkLocalState &lstate) const {
	if (BatchOrdered()) {
		auto min_batch_index = lstate.partition_info.min_batch_index.GetIndex();
		gstate.buffered_data->Cast<BatchedBufferedData>().UpdateMinBatchIndex(min_batch_index);
	}
	return SinkCombineResultType::FINISHED;
}

SinkCombineResultType PhysicalResultSink::CombineRetained(ClientContext &context, ResultSinkGlobalState &gstate,
                                                          ResultSinkLocalState &lstate) const {
	auto &buffered_data = *gstate.buffered_data;
	annotated_lock_guard<annotated_mutex> l(gstate.glock);
	if (!gstate.collection) {
		// Never a producer's own: Combine flushes the producer's partial unit here, inside its task, so a
		// throwing format surfaces as the query's error
		gstate.collection = buffered_data.Format().CreateCollection(context, buffered_data.FormatState(),
		                                                            buffered_data.FormatContext());
	}
	// A producer whose partition held no rows never created its local collection
	if (lstate.collection) {
		gstate.collection->Combine(*lstate.collection);
	}
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalResultSink::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                              OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<ResultSinkGlobalState>();
	if (CurrentLifetime(gstate) == ResultLifetime::DRAINING) {
		return SinkFinalizeType::READY;
	}
	auto &buffered_data = *gstate.buffered_data;
	annotated_lock_guard<annotated_mutex> l(gstate.glock);
	if (!gstate.collection) {
		// A query that sinks no rows can reach here still UNDECIDED: no producer ever parked, so an empty
		// deposit is correct whether the consumer goes on to retain or to drain instead
		gstate.collection = buffered_data.Format().CreateCollection(context, buffered_data.FormatState(),
		                                                            buffered_data.FormatContext());
	}
	gstate.collection->Finalize();
	return SinkFinalizeType::READY;
}

SinkNextBatchType PhysicalResultSink::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<ResultSinkGlobalState>();
	auto &lstate = input.local_state.Cast<ResultSinkLocalState>();
	// Flushed at the batch boundary, so no unit spans two batch indexes
	if (FlushUnits(gstate, lstate, input.interrupt_state)) {
		return SinkNextBatchType::BLOCKED;
	}
	if (!DrainsByBatchIndex(gstate)) {
		return SinkNextBatchType::READY;
	}

	auto batch = lstate.current_batch;
	auto min_batch_index = lstate.partition_info.min_batch_index.GetIndex();
	auto new_index = lstate.partition_info.batch_index.GetIndex();

	auto &buffered_data = gstate.buffered_data->Cast<BatchedBufferedData>();
	buffered_data.CompleteBatch(batch);
	lstate.current_batch = new_index;
	buffered_data.UpdateMinBatchIndex(min_batch_index);
	return SinkNextBatchType::READY;
}

SinkNextBatchType PhysicalResultSink::UpdateMinBatchIndex(ExecutionContext &context,
                                                          OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<ResultSinkGlobalState>();
	if (!DrainsByBatchIndex(gstate)) {
		return SinkNextBatchType::READY;
	}
	auto min_batch_index = input.local_state.partition_info.min_batch_index.GetIndex();
	gstate.buffered_data->Cast<BatchedBufferedData>().UpdateMinBatchIndex(min_batch_index);
	return SinkNextBatchType::READY;
}

unique_ptr<QueryResult> PhysicalResultSink::GetResult(GlobalSinkState &state) const {
	auto &gstate = state.Cast<ResultSinkGlobalState>();
	// A draining sink hands its units to the consumer through the buffer, never through a result
	D_ASSERT(CurrentLifetime(gstate) != ResultLifetime::DRAINING);
	auto cc = gstate.context.lock();
	if (!cc) {
		throw ConnectionException("Connection has already been closed");
	}
	auto &buffered_data = *gstate.buffered_data;
	unique_ptr<RetainedResultCollection> collection;
	{
		annotated_lock_guard<annotated_mutex> l(gstate.glock);
		collection = std::move(gstate.collection);
	}
	// Finalize already built and finalized it, inside the pipeline's finish task
	D_ASSERT(collection);
	return make_uniq<QueryResult>(statement_type, properties, types, names, std::move(collection),
	                              buffered_data.SharedFormat(), buffered_data.SharedFormatState(),
	                              cc->GetClientProperties());
}

OperatorPartitionInfo PhysicalResultSink::RequiredPartitionInfo() const {
	if (BatchOrdered()) {
		return OperatorPartitionInfo::BatchIndex();
	}
	return PhysicalOperator::RequiredPartitionInfo();
}

bool PhysicalResultSink::ParallelSink() const {
	// Source order is preserved by keeping the sink single-threaded
	return ordering != ResultOrdering::SOURCE_ORDERED;
}

bool PhysicalResultSink::SinkOrderDependent() const {
	return !BatchOrdered();
}

bool PhysicalResultSink::IsStreaming() const {
	// Producers may park: the buffer's lifetime decides whether they drain or retain
	return true;
}

PipelineExternalInputSupport PhysicalResultSink::GetExternalInputSupport() const {
	if (BatchOrdered()) {
		return PipelineExternalInputSupport::SUPPORTED;
	}
	return PhysicalOperator::GetExternalInputSupport();
}

} // namespace duckdb
