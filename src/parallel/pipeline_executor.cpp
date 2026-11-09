#include "duckdb/parallel/pipeline_executor.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/progress_verifier.hpp"

#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
#include <chrono>
#include <thread>
#endif

namespace duckdb {

#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
bool PipelineExecutor::TryDebugBlock(int &debug_counter, const InterruptState &interrupt_state_p) {
	if (debug_counter >= debug_blocked_target_count) {
		return false;
	}
	debug_counter++;
	std::thread rewake_thread([interrupt_state_p] {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		interrupt_state_p.Callback();
	});
	rewake_thread.detach();
	return true;
}
#endif

PipelineExecutor::PipelineExecutor(ClientContext &context_p, Pipeline &pipeline_p, optional_idx reserved_batch_index)
    : pipeline(pipeline_p), thread(context_p), context(context_p, thread, &pipeline_p) {
	if (pipeline.sink) {
		local_sink_state = pipeline.sink->GetLocalSinkState(context);
		required_partition_info = pipeline.sink->RequiredPartitionInfo();
		if (required_partition_info.AnyRequired()) {
			D_ASSERT(pipeline.source->SupportsPartitioning(required_partition_info));
			auto &partition_info = local_sink_state->partition_info;
			D_ASSERT(!partition_info.batch_index.IsValid());
			// batch index is not set yet - initialize before fetching anything
			if (pipeline.IsExternalInput()) {
				D_ASSERT(!reserved_batch_index.IsValid());
				partition_info.batch_index = pipeline.GetBaseBatchIndex();
			} else {
				partition_info.batch_index =
				    reserved_batch_index.IsValid() ? reserved_batch_index.GetIndex() : pipeline.RegisterNewBatchIndex();
			}
			partition_info.min_batch_index = partition_info.batch_index;
		}
	}
	if (!pipeline.IsExternalInput()) {
		global_source_state = pipeline.GetSourceState();
		D_ASSERT(global_source_state);
		local_source_state = pipeline.source->GetLocalSourceState(context, *global_source_state);
	}

	intermediate_chunks.reserve(pipeline.operators.size());
	intermediate_states.reserve(pipeline.operators.size());
	for (idx_t i = 0; i < pipeline.operators.size(); i++) {
		auto &prev_operator = i == 0 ? *pipeline.source : pipeline.operators[i - 1].get();
		auto &current_operator = pipeline.operators[i].get();

		auto chunk = make_uniq<DataChunk>();
		prev_operator.InitializeOutputChunk(BufferAllocator::Get(context.client), *chunk);
		intermediate_chunks.push_back(std::move(chunk));

		auto op_state = current_operator.GetOperatorState(context);
		intermediate_states.push_back(std::move(op_state));

		if (current_operator.IsSink() && current_operator.sink_state->state == SinkFinalizeType::NO_OUTPUT_POSSIBLE) {
			// one of the operators has already figured out no output is possible
			// we can skip executing the pipeline
			FinishProcessing();
		}
	}
	InitializeChunk(final_chunk);
}

void PipelineExecutor::Reset() {
	auto allow_reuse = Settings::Get<EnableCachingOperatorsSetting>(context.client);

	// Reset execution flags
	exhausted_pipeline = false;
	finalized = false;
	started_flushing = false;
	done_flushing = false;
	remaining_sink_chunk = false;
	next_batch_blocked = false;
	pending_batch_advance_state = PendingBatchAdvanceState::NONE;
	external_batch_initialized = false;
	finished_processing_idx = -1;
	source_profiling_finalized = false;
	source_finish_notification_state = SourceFinishNotificationState::PENDING;
	should_flush_current_idx = true;
	unsampled_progress_rows = 0;
	while (!in_process_operators.empty()) {
		in_process_operators.pop();
	}

	// Recreate local sink state (destroyed by PushFinalize)
	if (pipeline.sink) {
		if (!allow_reuse || !local_sink_state || !local_sink_state->SupportsReuse()) {
			local_sink_state = pipeline.sink->GetLocalSinkState(context);
		} else {
			local_sink_state->Reset(context, *pipeline.sink->sink_state);
		}
		required_partition_info = pipeline.sink->RequiredPartitionInfo();
		local_sink_state->partition_info = SourcePartitionInfo();
		if (required_partition_info.AnyRequired()) {
			D_ASSERT(pipeline.source->SupportsPartitioning(required_partition_info));
			auto &partition_info = local_sink_state->partition_info;
			D_ASSERT(!partition_info.batch_index.IsValid());
			partition_info.batch_index =
			    pipeline.IsExternalInput() ? pipeline.GetBaseBatchIndex() : pipeline.RegisterNewBatchIndex();
			partition_info.min_batch_index = partition_info.batch_index;
		}
	}

	if (pipeline.IsExternalInput()) {
		local_source_state.reset();
		global_source_state.reset();
	} else {
		global_source_state = pipeline.GetSourceState();
		D_ASSERT(global_source_state);
		// Recreate local source state (source data changed)
		if (!allow_reuse || !local_source_state || !local_source_state->SupportsReuse()) {
			local_source_state = pipeline.source->GetLocalSourceState(context, *global_source_state);
		} else {
			local_source_state->Reset(context, *global_source_state);
		}
	}

	// Recreate intermediate operator states (finalized in previous execution)
	// Keep intermediate_chunks — reuse their allocated memory
	for (idx_t i = 0; i < pipeline.operators.size(); i++) {
		auto &current_operator = pipeline.operators[i].get();
		if (!allow_reuse || !intermediate_states[i] || !intermediate_states[i]->SupportsReuse()) {
			intermediate_states[i] = current_operator.GetOperatorState(context);
		} else {
			intermediate_states[i]->Reset();
		}

		if (current_operator.IsSink() && current_operator.sink_state->state == SinkFinalizeType::NO_OUTPUT_POSSIBLE) {
			FinishProcessing();
		}
	}

	// Reset the final chunk data (keep allocation)
	final_chunk.Reset();
}

void PipelineExecutor::PrepareForExecution() {
	if (!has_executed) {
		has_executed = true;
		return;
	}
	Reset();
}

bool PipelineExecutor::TryFlushCachingOperators(ExecutionBudget &chunk_budget) {
	if (!started_flushing) {
		// Remainder of this method assumes any in process operators are from flushing
		D_ASSERT(in_process_operators.empty());
		started_flushing = true;
		flushing_idx = IsFinished() ? idx_t(finished_processing_idx) : 0;
	}

	// For each operator that supports FinalExecute,
	// extract every chunk from it and push it through the rest of the pipeline
	// before moving onto the next operators' FinalExecute
	while (flushing_idx < pipeline.operators.size()) {
		if (!pipeline.operators[flushing_idx].get().RequiresFinalExecute()) {
			flushing_idx++;
			continue;
		}

		// This slightly awkward way of increasing the flushing idx is to make the code re-entrant: We need to call this
		// method again in the case of a Sink returning BLOCKED.
		if (!should_flush_current_idx && in_process_operators.empty()) {
			should_flush_current_idx = true;
			flushing_idx++;
			continue;
		}

		auto &curr_chunk =
		    flushing_idx + 1 >= intermediate_chunks.size() ? final_chunk : *intermediate_chunks[flushing_idx + 1];
		auto &current_operator = pipeline.operators[flushing_idx].get();

		// In-process operators here mean an earlier push of curr_chunk was interrupted (the chunk budget
		// ran out, or the Sink blocked). We resume that push rather than flushing the operator again, and
		// leave should_flush_current_idx alone: it still reflects the last actual FinalExecute.
		const bool resuming_push = !in_process_operators.empty();

		if (!resuming_push) {
			curr_chunk.Reset();
			StartOperator(current_operator);
			auto finalize_result = current_operator.FinalExecute(context, curr_chunk, *current_operator.op_state,
			                                                     *intermediate_states[flushing_idx]);
			EndOperator(current_operator, &curr_chunk);
			should_flush_current_idx = finalize_result == OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
		}

		auto push_result = ExecutePushInternal(curr_chunk, chunk_budget, flushing_idx + 1);

		switch (push_result) {
		case OperatorResultType::BLOCKED: {
			remaining_sink_chunk = true;
			return false;
		}
		case OperatorResultType::HAVE_MORE_OUTPUT: {
			D_ASSERT(chunk_budget.IsDepleted());
			// The chunk budget was used up, pushing the chunk through the pipeline created more chunks
			// we need to continue this the next time Execute is called.
			return false;
		}
		case OperatorResultType::NEED_MORE_INPUT:
		case OperatorResultType::FINISHED:
			break;
		default:
			throw InternalException("Unexpected OperatorResultType (%s) in TryFlushCachingOperators",
			                        EnumUtil::ToString(push_result));
		}
	}
	return true;
}

SinkNextBatchType PipelineExecutor::NextBatch(DataChunk &source_chunk, const SourceFetchResult &source_result) {
	D_ASSERT(required_partition_info.AnyRequired());
	auto max_batch_index = pipeline.base_batch_index + PipelineBuildState::BATCH_INCREMENT - 1;
	// by default set it to the maximum valid batch index value for the current pipeline
	auto &partition_info = local_sink_state->partition_info;
	OperatorPartitionData next_data(max_batch_index);
	if (source_chunk.size() > 0 || source_result.batch_index_state == SourceBatchIndexState::ADVANCED) {
		D_ASSERT(local_source_state);
		D_ASSERT(global_source_state);
		// Read the source batch for both data chunks and explicit empty batch advances.
		auto source_data = pipeline.source->GetPartitionData(context, source_chunk, *global_source_state,
		                                                     *local_source_state, required_partition_info);
		next_data = ToPipelinePartitionData(source_data);
	} else if (source_result.result == SourceResultType::HAVE_MORE_OUTPUT) {
		next_data.batch_index = partition_info.batch_index.GetIndex();
	}
	return NextBatch(std::move(next_data), source_result.batch_index_state == SourceBatchIndexState::ADVANCED);
}

OperatorPartitionData PipelineExecutor::ToPipelinePartitionData(const OperatorPartitionData &source_data) const {
	auto max_batch_index = pipeline.base_batch_index + PipelineBuildState::BATCH_INCREMENT - 1;
	OperatorPartitionData result(pipeline.base_batch_index + source_data.batch_index + 1);
	result.partition_data = source_data.partition_data;
	if (result.batch_index >= max_batch_index) {
		throw InternalException("Pipeline batch index - invalid batch index %llu returned by source operator",
		                        source_data.batch_index);
	}
	return result;
}

idx_t PipelineExecutor::MapExternalMinBatchIndex(optional_idx source_min_batch_index) const {
	D_ASSERT(pipeline.IsExternalInput());
	if (!source_min_batch_index.IsValid()) {
		throw InternalException("External pipeline did not provide a minimum batch index");
	}
	auto min_batch_offset = source_min_batch_index.GetIndex();
	if (min_batch_offset >= PipelineBuildState::BATCH_INCREMENT) {
		throw InternalException("External pipeline minimum batch index is outside its pipeline");
	}
	auto min_batch_index = pipeline.GetBaseBatchIndex() + min_batch_offset;
	auto &partition_info = local_sink_state->partition_info;
	if (min_batch_index < partition_info.min_batch_index.GetIndex()) {
		throw InternalException("External pipeline minimum batch index decreased from %llu to %llu",
		                        partition_info.min_batch_index.GetIndex(), min_batch_index);
	}
	return min_batch_index;
}

SinkNextBatchType PipelineExecutor::NextBatch(OperatorPartitionData next_data, bool force,
                                              optional_idx external_min_batch_index) {
	auto &partition_info = local_sink_state->partition_info;
	optional_idx mapped_external_min_batch_index;
	if (pipeline.IsExternalInput()) {
		auto min_batch_index = MapExternalMinBatchIndex(external_min_batch_index);
		if (min_batch_index > next_data.batch_index) {
			throw InternalException("External pipeline minimum batch index %llu exceeds current batch index %llu",
			                        min_batch_index, next_data.batch_index);
		}
		mapped_external_min_batch_index = min_batch_index;
	} else if (external_min_batch_index.IsValid()) {
		throw InternalException("Source-driven pipeline received an external minimum batch index");
	}
	if (!force && next_data.batch_index == partition_info.batch_index.GetIndex()) {
		if (mapped_external_min_batch_index.IsValid()) {
			partition_info.min_batch_index = mapped_external_min_batch_index;
		}
		// no changes, return
		return SinkNextBatchType::READY;
	}
	// batch index has changed - update it
	if (partition_info.batch_index.GetIndex() > next_data.batch_index) {
		throw InternalException(
		    "Pipeline batch index - gotten lower batch index %llu (down from previous batch index of %llu)",
		    next_data.batch_index, partition_info.batch_index.GetIndex());
	}
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	if (TryDebugBlock(debug_blocked_next_batch_count, interrupt_state)) {
		return SinkNextBatchType::BLOCKED;
	}
#endif
	auto current_batch = partition_info.batch_index.GetIndex();
	partition_info.batch_index = next_data.batch_index;
	partition_info.partition_data = std::move(next_data.partition_data);
	OperatorSinkNextBatchInput next_batch_input {*pipeline.sink->sink_state, *local_sink_state, interrupt_state};
	// call NextBatch before updating min_batch_index to provide the opportunity to flush the previous batch
	auto next_batch_result = pipeline.sink->NextBatch(context, next_batch_input);

	if (next_batch_result == SinkNextBatchType::BLOCKED) {
		partition_info.batch_index = current_batch; // set batch_index back to what it was before
		return SinkNextBatchType::BLOCKED;
	}

	if (pipeline.IsExternalInput()) {
		D_ASSERT(mapped_external_min_batch_index.IsValid());
		partition_info.min_batch_index = mapped_external_min_batch_index;
	} else {
		partition_info.min_batch_index = pipeline.UpdateBatchIndex(current_batch, next_data.batch_index);
	}

	return SinkNextBatchType::READY;
}

PipelineExecuteResult PipelineExecutor::Execute(idx_t max_chunks) {
	D_ASSERT(pipeline.sink);
	auto &source_chunk = pipeline.operators.empty() ? final_chunk : *intermediate_chunks[0];
	ExecutionBudget chunk_budget(max_chunks);

	do {
		context.client.InterruptCheck();
		switch (pending_batch_advance_state) {
		case PendingBatchAdvanceState::MINIMUM_UPDATE:
			if (UpdateMinBatchIndex() == SinkNextBatchType::BLOCKED) {
				return PipelineExecuteResult::INTERRUPTED;
			}
			pending_batch_advance_state = PendingBatchAdvanceState::NONE;
			continue;
		case PendingBatchAdvanceState::NEXT_BATCH: {
			D_ASSERT(source_chunk.size() == 0);
			SourceFetchResult source_result;
			source_result.batch_index_state = SourceBatchIndexState::ADVANCED;
			auto next_batch_result = NextBatch(source_chunk, source_result);
			if (next_batch_result == SinkNextBatchType::BLOCKED) {
				return PipelineExecuteResult::INTERRUPTED;
			}
			pending_batch_advance_state = PendingBatchAdvanceState::MINIMUM_UPDATE;
			continue;
		}
		case PendingBatchAdvanceState::NONE:
			break;
		}

		OperatorResultType result;
		if (exhausted_pipeline && done_flushing && !remaining_sink_chunk && !next_batch_blocked &&
		    in_process_operators.empty()) {
			break;
		} else if (remaining_sink_chunk) {
			// The pipeline was interrupted by the Sink. We should retry sinking the final chunk.
			auto push_result = PushInputChunk(source_chunk, chunk_budget, PipelineInputChunkMode::RESUME_INPUT);
			if (push_result == PipelineExecuteResult::INTERRUPTED) {
				return PipelineExecuteResult::INTERRUPTED;
			}
			result = push_result == PipelineExecuteResult::FINISHED ? OperatorResultType::FINISHED
			                                                        : OperatorResultType::NEED_MORE_INPUT;
		} else if (!in_process_operators.empty() && !started_flushing) {
			// Operator(s) in the pipeline have returned `HAVE_MORE_OUTPUT` in the last Execute call
			// the operators have to be called with the same input chunk to produce the rest of the output
			D_ASSERT(source_chunk.size() > 0);
			result = ExecutePushInternal(source_chunk, chunk_budget);
		} else if (exhausted_pipeline && (!next_batch_blocked || done_flushing)) {
			// The pipeline was exhausted, try flushing all operators
			return FlushAndFinalize(chunk_budget);
		} else if (!exhausted_pipeline || next_batch_blocked) {
			SourceFetchResult source_result;
			if (!next_batch_blocked) {
				// "Regular" path: fetch a chunk from the source and push it through the pipeline
				source_chunk.Reset();
				source_result = FetchFromSource(source_chunk);
				if (source_result.batch_index_state == SourceBatchIndexState::ADVANCED) {
					D_ASSERT(source_result.result == SourceResultType::BLOCKED);
					D_ASSERT(required_partition_info.RequiresBatchIndex());
					D_ASSERT(!required_partition_info.RequiresPartitionColumns());
					pending_batch_advance_state = PendingBatchAdvanceState::NEXT_BATCH;
					continue;
				}
				if (source_result.result == SourceResultType::BLOCKED) {
					return PipelineExecuteResult::INTERRUPTED;
				}
				if (source_result.result == SourceResultType::FINISHED) {
					exhausted_source = true;
					exhausted_pipeline = true;
				}
			}

			if (required_partition_info.AnyRequired() &&
			    (source_result.result != SourceResultType::FINISHED || source_chunk.size() > 0)) {
				auto next_batch_result = NextBatch(source_chunk, source_result);
				next_batch_blocked = next_batch_result == SinkNextBatchType::BLOCKED;
				if (next_batch_blocked) {
					return PipelineExecuteResult::INTERRUPTED;
				}
			}

			if (exhausted_pipeline && source_chunk.size() == 0) {
				continue;
			}

			result = ExecutePushInternal(source_chunk, chunk_budget);
		} else {
			throw InternalException("Unexpected state reached in pipeline executor");
		}

		// SINK INTERRUPT
		if (result == OperatorResultType::BLOCKED) {
			remaining_sink_chunk = true;
			return PipelineExecuteResult::INTERRUPTED;
		}

		if (result == OperatorResultType::FINISHED) {
			D_ASSERT(in_process_operators.empty());
			exhausted_pipeline = true;
		}
	} while (chunk_budget.Next());

	if ((!exhausted_pipeline || !done_flushing) && !IsFinished()) {
		return PipelineExecuteResult::NOT_FINISHED;
	}

	return PushFinalize();
}

PipelineExecuteResult PipelineExecutor::Execute() {
	return Execute(NumericLimits<idx_t>::Maximum());
}

PipelineExecuteResult PipelineExecutor::PushExternal(DataChunk &input,
                                                     const OperatorPartitionData &source_partition_data,
                                                     optional_idx source_min_batch_index) {
	D_ASSERT(pipeline.sink);
	D_ASSERT(pipeline.IsExternalInput());
	if (IsFinished()) {
		return PipelineExecuteResult::FINISHED;
	}
	if (required_partition_info.AnyRequired() && !remaining_sink_chunk) {
		auto next_batch_result = NextBatchExternal(source_partition_data, source_min_batch_index);
		if (next_batch_result != PipelineExecuteResult::NOT_FINISHED) {
			return next_batch_result;
		}
	}
	if (!remaining_sink_chunk && !next_batch_blocked) {
		context.thread.profiler.StartOperator(pipeline.source.get());
		context.thread.profiler.EndOperator(&input);
	}

	ExecutionBudget chunk_budget(NumericLimits<idx_t>::Maximum());
	return PushInputChunk(input, chunk_budget, PipelineInputChunkMode::PUSH_INPUT);
}

PipelineExecuteResult PipelineExecutor::NextBatchExternal(const OperatorPartitionData &source_partition_data,
                                                          optional_idx source_min_batch_index) {
	D_ASSERT(pipeline.sink);
	D_ASSERT(pipeline.IsExternalInput());
	if (IsFinished()) {
		return PipelineExecuteResult::FINISHED;
	}
	if (!required_partition_info.AnyRequired()) {
		return PipelineExecuteResult::NOT_FINISHED;
	}
	auto next_batch_result =
	    NextBatch(ToPipelinePartitionData(source_partition_data), !external_batch_initialized, source_min_batch_index);
	next_batch_blocked = next_batch_result == SinkNextBatchType::BLOCKED;
	if (next_batch_blocked) {
		return PipelineExecuteResult::INTERRUPTED;
	}
	external_batch_initialized = true;
	return PipelineExecuteResult::NOT_FINISHED;
}

PipelineExecuteResult PipelineExecutor::FinishExternal(optional_idx source_min_batch_index) {
	D_ASSERT(pipeline.sink);
	D_ASSERT(pipeline.IsExternalInput());
	if (finalized) {
		return PipelineExecuteResult::FINISHED;
	}
	auto finish_batch_result = FinishBatchExternal(source_min_batch_index);
	if (finish_batch_result == PipelineExecuteResult::INTERRUPTED) {
		return finish_batch_result;
	}

	ExecutionBudget chunk_budget(NumericLimits<idx_t>::Maximum());
	exhausted_source = true;
	exhausted_pipeline = true;
	return FlushAndFinalize(chunk_budget);
}

PipelineExecuteResult PipelineExecutor::FinishBatchExternal(optional_idx source_min_batch_index) {
	D_ASSERT(pipeline.sink);
	D_ASSERT(pipeline.IsExternalInput());
	if (finalized) {
		return PipelineExecuteResult::FINISHED;
	}
	if (required_partition_info.AnyRequired()) {
		auto max_batch_index = pipeline.base_batch_index + PipelineBuildState::BATCH_INCREMENT - 1;
		auto next_batch_result =
		    NextBatch(OperatorPartitionData(max_batch_index), !external_batch_initialized, source_min_batch_index);
		next_batch_blocked = next_batch_result == SinkNextBatchType::BLOCKED;
		if (next_batch_blocked) {
			return PipelineExecuteResult::INTERRUPTED;
		}
		external_batch_initialized = true;
	}
	return PipelineExecuteResult::NOT_FINISHED;
}

PipelineExecuteResult PipelineExecutor::UpdateMinBatchIndexExternal(optional_idx source_min_batch_index) {
	D_ASSERT(pipeline.sink);
	D_ASSERT(pipeline.IsExternalInput());
	if (IsFinished() || !required_partition_info.RequiresBatchIndex()) {
		return IsFinished() ? PipelineExecuteResult::FINISHED : PipelineExecuteResult::NOT_FINISHED;
	}
	auto min_batch_index = MapExternalMinBatchIndex(source_min_batch_index);
	auto &partition_info = local_sink_state->partition_info;
	partition_info.min_batch_index = min_batch_index;
	if (UpdateMinBatchIndex() == SinkNextBatchType::BLOCKED) {
		return PipelineExecuteResult::INTERRUPTED;
	}
	return PipelineExecuteResult::NOT_FINISHED;
}

bool PipelineExecutor::IsFinishedProcessing() const {
	return IsFinished();
}

void PipelineExecutor::FinishProcessing(int32_t operator_idx) {
	finished_processing_idx = operator_idx < 0 ? NumericLimits<int32_t>::Maximum() : operator_idx;
	in_process_operators = stack<idx_t>();

	pipeline.PreventBlocking();
}

void PipelineExecutor::NotifySourceFinished() {
	if (source_finish_notification_state == SourceFinishNotificationState::SENT || pipeline.IsExternalInput() ||
	    !pipeline.GetSource() || !global_source_state) {
		return;
	}
	source_finish_notification_state = SourceFinishNotificationState::SENT;
	pipeline.source->SourceFinished(context.client, *global_source_state);
}

bool PipelineExecutor::IsFinished() const {
	return finished_processing_idx >= 0;
}

OperatorResultType PipelineExecutor::ExecutePushInternal(DataChunk &input, ExecutionBudget &chunk_budget,
                                                         idx_t initial_idx) {
	D_ASSERT(pipeline.sink);
	if (input.size() == 0) { // LCOV_EXCL_START
		return OperatorResultType::NEED_MORE_INPUT;
	} // LCOV_EXCL_STOP

	// this loop will continuously push the input chunk through the pipeline as long as:
	// - the OperatorResultType for the Execute is HAVE_MORE_OUTPUT
	// - the Sink doesn't block
	// - the ExecutionBudget has not been depleted
	OperatorResultType result = OperatorResultType::HAVE_MORE_OUTPUT;
	do {
		// Note: if input is the final_chunk, we don't do any executing, the chunk just needs to be sinked
		if (&input != &final_chunk) {
			final_chunk.Reset();
			if (pipeline.operators.empty()) {
				final_chunk.Reference(input);
				result = OperatorResultType::NEED_MORE_INPUT;
			} else {
				// Execute and put the result into 'final_chunk'
				result = Execute(input, final_chunk, initial_idx);
				if (result == OperatorResultType::FINISHED) {
					return OperatorResultType::FINISHED;
				}
			}
		} else {
			result = OperatorResultType::NEED_MORE_INPUT;
		}
		auto &sink_chunk = final_chunk;
		if (sink_chunk.size() > 0) {
			StartOperator(*pipeline.sink);
			D_ASSERT(pipeline.sink);
			D_ASSERT(pipeline.sink->sink_state);
			OperatorSinkInput sink_input {*pipeline.sink->sink_state, *local_sink_state, interrupt_state};

			auto sink_result = Sink(sink_chunk, sink_input);

			EndOperator(*pipeline.sink, nullptr);

			if (sink_result == SinkResultType::BLOCKED) {
				return OperatorResultType::BLOCKED;
			} else if (sink_result == SinkResultType::FINISHED) {
				FinishProcessing();
				return OperatorResultType::FINISHED;
			}
		}
		if (result == OperatorResultType::NEED_MORE_INPUT) {
			return OperatorResultType::NEED_MORE_INPUT;
		}
	} while (chunk_budget.Next());
	return result;
}

PipelineExecuteResult PipelineExecutor::PushInputChunk(DataChunk &input, ExecutionBudget &chunk_budget,
                                                       PipelineInputChunkMode mode) {
	if (remaining_sink_chunk) {
		auto result = ExecutePushInternal(final_chunk, chunk_budget);
		D_ASSERT(result != OperatorResultType::HAVE_MORE_OUTPUT);
		if (result == OperatorResultType::BLOCKED) {
			return PipelineExecuteResult::INTERRUPTED;
		}
		remaining_sink_chunk = false;
		if (result == OperatorResultType::FINISHED) {
			return PipelineExecuteResult::FINISHED;
		}
		if (mode == PipelineInputChunkMode::RESUME_INPUT || in_process_operators.empty()) {
			return PipelineExecuteResult::NOT_FINISHED;
		}
	}
	if (mode == PipelineInputChunkMode::RESUME_INPUT) {
		return PipelineExecuteResult::NOT_FINISHED;
	}

	while (true) {
		context.client.InterruptCheck();
		auto result = ExecutePushInternal(input, chunk_budget);
		if (result == OperatorResultType::BLOCKED) {
			remaining_sink_chunk = true;
			return PipelineExecuteResult::INTERRUPTED;
		}
		if (result == OperatorResultType::FINISHED) {
			D_ASSERT(in_process_operators.empty());
			return PipelineExecuteResult::FINISHED;
		}
		if (result != OperatorResultType::HAVE_MORE_OUTPUT) {
			return PipelineExecuteResult::NOT_FINISHED;
		}
	}
}

PipelineExecuteResult PipelineExecutor::FlushAndFinalize(ExecutionBudget &chunk_budget) {
	if (remaining_sink_chunk) {
		auto result = ExecutePushInternal(final_chunk, chunk_budget);
		D_ASSERT(result != OperatorResultType::HAVE_MORE_OUTPUT);
		if (result == OperatorResultType::BLOCKED) {
			return PipelineExecuteResult::INTERRUPTED;
		}
		remaining_sink_chunk = false;
		if (result == OperatorResultType::FINISHED) {
			exhausted_pipeline = true;
		}
	}
	if (!done_flushing) {
		auto flush_completed = TryFlushCachingOperators(chunk_budget);
		if (!flush_completed) {
			if (remaining_sink_chunk) {
				return PipelineExecuteResult::INTERRUPTED;
			}
			D_ASSERT(chunk_budget.IsDepleted());
			return PipelineExecuteResult::NOT_FINISHED;
		}
		done_flushing = true;
	}
	if (required_partition_info.AnyRequired() && !pipeline.IsExternalInput()) {
		DataChunk empty_chunk;
		SourceFetchResult source_result;
		source_result.result = SourceResultType::FINISHED;
		auto next_batch_result = NextBatch(empty_chunk, source_result);
		next_batch_blocked = next_batch_result == SinkNextBatchType::BLOCKED;
		if (next_batch_blocked) {
			return PipelineExecuteResult::INTERRUPTED;
		}
	}
	return PushFinalize();
}

PipelineExecuteResult PipelineExecutor::PushFinalize() {
	if (finalized) {
		throw InternalException("Calling PushFinalize on a pipeline that has been finalized already");
	}

	D_ASSERT(local_sink_state);

	// Run the combine for the sink
	OperatorSinkCombineInput combine_input {*pipeline.sink->sink_state, *local_sink_state, interrupt_state};

#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	if (TryDebugBlock(debug_blocked_combine_count, combine_input.interrupt_state)) {
		return PipelineExecuteResult::INTERRUPTED;
	}
#endif
	auto result = pipeline.sink->Combine(context, combine_input);

	if (result == SinkCombineResultType::BLOCKED) {
		return PipelineExecuteResult::INTERRUPTED;
	}

	finalized = true;
	NotifySourceFinished();

	SampleProgress();
	auto progress_verifier = pipeline.executor.GetProgressVerifier();
	if (progress_verifier && !exhausted_source && !pipeline.IsExternalInput()) {
		progress_verifier->OnEarlyExit(pipeline);
	}

	// If source was not exhausted (e.g. LIMIT stopped the pipeline), collect exact metrics now
	if (!source_profiling_finalized && local_source_state && global_source_state) {
		context.thread.profiler.FinishSource(*pipeline.source, *global_source_state, *local_source_state);
	}

	// flush all query profiler info
	for (idx_t i = 0; i < intermediate_states.size(); i++) {
		intermediate_states[i]->Finalize(pipeline.operators[i].get(), context);
	}
	pipeline.executor.Flush(thread);

	return PipelineExecuteResult::FINISHED;
}

void PipelineExecutor::GoToSource(idx_t &current_idx, idx_t initial_idx) {
	// we go back to the first operator (the source)
	current_idx = initial_idx;
	if (!in_process_operators.empty()) {
		// ... UNLESS there is an in process operator
		// if there is an in-process operator, we start executing at the latest one
		// for example, if we have a join operator that has tuples left, we first need to emit those tuples
		current_idx = in_process_operators.top();
		in_process_operators.pop();
	}
	D_ASSERT(current_idx >= initial_idx);
}

OperatorResultType PipelineExecutor::Execute(DataChunk &input, DataChunk &result, idx_t initial_idx) {
	if (input.size() == 0) { // LCOV_EXCL_START
		return OperatorResultType::NEED_MORE_INPUT;
	} // LCOV_EXCL_STOP
	D_ASSERT(!pipeline.operators.empty());

	idx_t current_idx;
	GoToSource(current_idx, initial_idx);
	if (current_idx == initial_idx) {
		current_idx++;
	}
	if (current_idx > pipeline.operators.size()) {
		result.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	while (true) {
		context.client.InterruptCheck();
		// now figure out where to put the chunk
		// if current_idx is the last possible index (>= operators.size()) we write to the result
		// otherwise we write to an intermediate chunk
		auto current_intermediate = current_idx;
		auto &current_chunk =
		    current_intermediate >= intermediate_chunks.size() ? result : *intermediate_chunks[current_intermediate];
		current_chunk.Reset();
		if (current_idx == initial_idx) {
			// we went back to the source: we need more input
			return OperatorResultType::NEED_MORE_INPUT;
		} else {
			auto &prev_chunk =
			    current_intermediate == initial_idx + 1 ? input : *intermediate_chunks[current_intermediate - 1];
			auto operator_idx = current_idx - 1;
			auto &current_operator = pipeline.operators[operator_idx].get();

			// if current_idx > source_idx, we pass the previous operators' output through the Execute of the current
			// operator
			StartOperator(current_operator);
			auto result = current_operator.Execute(context, prev_chunk, current_chunk, *current_operator.op_state,
			                                       *intermediate_states[current_intermediate - 1]);
			EndOperator(current_operator, &current_chunk);
			if (result == OperatorResultType::HAVE_MORE_OUTPUT) {
				// more data remains in this operator
				// push in-process marker
				in_process_operators.push(current_idx);
			} else if (result == OperatorResultType::FINISHED) {
				D_ASSERT(current_chunk.size() == 0);
				FinishProcessing(NumericCast<int32_t>(current_idx));
				return OperatorResultType::FINISHED;
			}
			current_chunk.Verify(context.client.db);
		}

		if (current_chunk.size() == 0) {
			// no output from this operator!
			if (current_idx == initial_idx) {
				// if we got no output from the scan, we are done
				break;
			} else {
				// if we got no output from an intermediate op
				// we go back and try to pull data from the source again
				GoToSource(current_idx, initial_idx);
				continue;
			}
		} else {
			// we got output! continue to the next operator
			current_idx++;
			if (current_idx > pipeline.operators.size()) {
				// if we got output and are at the last operator, we are finished executing for this output chunk
				// return the data and push it into the chunk
				break;
			}
		}
	}
	return in_process_operators.empty() ? OperatorResultType::NEED_MORE_INPUT : OperatorResultType::HAVE_MORE_OUTPUT;
}

void PipelineExecutor::SetTaskForInterrupts(weak_ptr<Task> current_task) {
	SetInterruptState(InterruptState(std::move(current_task)));
}

void PipelineExecutor::SetInterruptState(InterruptState interrupt_state_p) {
	interrupt_state = std::move(interrupt_state_p);
}

SourceResultType PipelineExecutor::GetData(DataChunk &chunk, OperatorSourceInput &input) {
	//! Testing feature to enable async source on every operator
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	if (TryDebugBlock(debug_blocked_source_count, input.interrupt_state)) {
		return SourceResultType::BLOCKED;
	}
#endif

	return pipeline.source->GetData(context, chunk, input);
}

SinkResultType PipelineExecutor::Sink(DataChunk &chunk, OperatorSinkInput &input) {
	//! Testing feature to enable async sink on every operator
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	if (TryDebugBlock(debug_blocked_sink_count, input.interrupt_state)) {
		return SinkResultType::BLOCKED;
	}
#endif
	return pipeline.sink->Sink(context, chunk, input);
}

SinkNextBatchType PipelineExecutor::UpdateMinBatchIndex() {
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	if (TryDebugBlock(debug_blocked_min_batch_count, interrupt_state)) {
		return SinkNextBatchType::BLOCKED;
	}
#endif
	OperatorSinkNextBatchInput input {*pipeline.sink->sink_state, *local_sink_state, interrupt_state};
	return pipeline.sink->UpdateMinBatchIndex(context, input);
}

PipelineExecutor::SourceFetchResult PipelineExecutor::FetchFromSource(DataChunk &result) {
	D_ASSERT(!pipeline.IsExternalInput());
	D_ASSERT(global_source_state);
	D_ASSERT(local_source_state);
	// the previous chunk has been pushed through the pipeline
	SampleProgress();
	StartOperator(*pipeline.source);

	OperatorSourceInput source_input = {*global_source_state, *local_source_state, interrupt_state};
	SourceFetchResult fetch_result;
	fetch_result.result = GetData(result, source_input);
	fetch_result.batch_index_state = source_input.batch_index_state;

	D_ASSERT(fetch_result.result != SourceResultType::BLOCKED || result.size() == 0);
	D_ASSERT(fetch_result.batch_index_state != SourceBatchIndexState::ADVANCED ||
	         fetch_result.result == SourceResultType::BLOCKED);
	if (fetch_result.result == SourceResultType::FINISHED) {
		// final call into the source - finish source execution
		NotifySourceFinished();
		context.thread.profiler.FinishSource(*global_source_state, *local_source_state);
		source_profiling_finalized = true;
	}
	EndOperator(*pipeline.source, &result);

	if (pipeline.executor.GetProgressVerifier()) {
		unsampled_progress_rows = result.size();
	}
	return fetch_result;
}

void PipelineExecutor::SampleProgress() {
	auto progress_verifier = pipeline.executor.GetProgressVerifier();
	if (!progress_verifier || unsampled_progress_rows == 0) {
		return;
	}
	progress_verifier->OnSourceChunk(pipeline, unsampled_progress_rows);
	unsampled_progress_rows = 0;
}

void PipelineExecutor::InitializeChunk(DataChunk &chunk) {
	auto &last_op = pipeline.operators.empty() ? *pipeline.source : pipeline.operators.back().get();
	last_op.InitializeOutputChunk(BufferAllocator::Get(context.client), chunk);
}

void PipelineExecutor::StartOperator(PhysicalOperator &op) {
	context.client.InterruptCheck();
	context.thread.profiler.StartOperator(&op);
}

void PipelineExecutor::EndOperator(PhysicalOperator &op, optional_ptr<DataChunk> chunk) {
	context.thread.profiler.EndOperator(chunk);

	if (chunk) {
		chunk->Verify(context.client.db);
	}
}

} // namespace duckdb
