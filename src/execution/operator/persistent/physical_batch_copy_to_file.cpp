#include "duckdb/execution/operator/persistent/physical_batch_copy_to_file.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/types/batched_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/persistent/batch_memory_manager.hpp"
#include "duckdb/execution/operator/persistent/batch_task_manager.hpp"
#include "duckdb/execution/operator/persistent/copy_batch_slicer.hpp"
#include "duckdb/execution/operator/persistent/copy_output_lifecycle.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/logging/log_type.hpp"

#include <algorithm>

namespace duckdb {

struct ActiveFlushGuard {
	explicit ActiveFlushGuard(atomic<bool> &bool_value_p) : bool_value(bool_value_p) {
		bool_value = true;
	}
	~ActiveFlushGuard() {
		bool_value = false;
	}

	atomic<bool> &bool_value;
};

PhysicalBatchCopyToFile::PhysicalBatchCopyToFile(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                 CopyFunction function_p, unique_ptr<FunctionData> bind_data_p,
                                                 idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::BATCH_COPY_TO_FILE, std::move(types),
                       estimated_cardinality),
      function(std::move(function_p)), bind_data(std::move(bind_data_p)) {
	if (!function.flush_batch || !function.prepare_batch) {
		throw InternalException("PhysicalFixedBatchCopy created for copy function that does not have "
		                        "prepare_batch/flush_batch defined");
	}
}

InsertionOrderPreservingMap<string> PhysicalBatchCopyToFile::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["FORMAT"] = StringUtil::Upper(function.name.GetIdentifierName());
	return result;
}

OperatorPartitionInfo PhysicalBatchCopyToFile::RequiredPartitionInfo() const {
	return OperatorPartitionInfo::BatchIndex(batch_size.IsValid() ? batch_size : optional_idx(DEFAULT_ROW_GROUP_SIZE));
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class BatchCopyTask {
public:
	virtual ~BatchCopyTask() {
	}

	virtual void Execute(const PhysicalBatchCopyToFile &op, ClientContext &context, GlobalSinkState &gstate_p) = 0;
};

struct FixedRawBatchData {
	FixedRawBatchData(idx_t memory_usage_p, unique_ptr<ColumnDataCollection> collection_p)
	    : memory_usage(memory_usage_p), collection(std::move(collection_p)) {
	}

	idx_t memory_usage;
	unique_ptr<ColumnDataCollection> collection;
};

struct FixedPreparedBatchData {
	idx_t memory_usage;
	unique_ptr<PreparedBatchData> prepared_data;
	CopyFunctionBatchAnalyzer batch_analyzer;
};

class FixedBatchCopyGlobalState : public GlobalSinkState {
public:
	// heuristic - we need at least 4MB of cache space per column per thread we launch
	static constexpr const idx_t MINIMUM_MEMORY_PER_COLUMN_PER_THREAD = 4ULL * 1024ULL * 1024ULL;

public:
	explicit FixedBatchCopyGlobalState(ClientContext &context_p, idx_t minimum_memory_per_thread)
	    : output_lifecycle(context_p), memory_manager(context_p, minimum_memory_per_thread), rows_copied(0),
	      scheduled_batch_index(0), flushed_batch_index(0), any_flushing(false), any_finished(false),
	      minimum_memory_per_thread(minimum_memory_per_thread) {
	}

	//! Destroy after global_state so its handle closes before cleanup.
	CopyOutputLifecycle output_lifecycle;
	BatchMemoryManager memory_manager;
	BatchTaskManager<BatchCopyTask> task_manager;
	mutex lock;
	mutex flush_lock;
	atomic<bool> initialized {false};
	//! The total number of rows copied to the file
	atomic<idx_t> rows_copied;
	//! Global copy state
	unique_ptr<GlobalFunctionData> global_state;
	//! Unpartitioned batches
	map<idx_t, unique_ptr<FixedRawBatchData>> raw_batches;
	//! The prepared batch data by batch index - ready to flush
	map<idx_t, unique_ptr<FixedPreparedBatchData>> batch_data;
	//! The index of the latest batch index that has been scheduled
	atomic<idx_t> scheduled_batch_index;
	//! The index of the latest batch index that has been flushed
	atomic<idx_t> flushed_batch_index;
	//! Whether or not any thread is flushing
	atomic<bool> any_flushing;
	//! Whether or not any threads are finished
	atomic<bool> any_finished;
	//! Minimum memory per thread
	idx_t minimum_memory_per_thread;
	//! Written file info (for RETURN_STATS)
	unique_ptr<CopyToFileInfo> written_file_info;

	void Initialize(ClientContext &context, const PhysicalBatchCopyToFile &op) {
		if (initialized.load(std::memory_order_acquire)) {
			return;
		}
		lock_guard<mutex> guard(lock);
		if (initialized.load(std::memory_order_relaxed)) {
			return;
		}
		output_lifecycle.RegisterFile(op.file_path);
		auto new_global_state = op.function.copy_to_initialize_global(context, *op.bind_data, op.file_path);
		if (op.function.initialize_operator) {
			op.function.initialize_operator(*new_global_state, op);
		}
		unique_ptr<CopyToFileInfo> new_written_file_info;
		if (GetCopyFunctionBaseReturnType(op.return_type) == CopyFunctionReturnType::WRITTEN_FILE_STATISTICS) {
			new_written_file_info = make_uniq<CopyToFileInfo>(op.file_path);
			new_written_file_info->file_stats = make_uniq<CopyFunctionFileStatistics>();
			op.function.copy_to_get_written_statistics(context, *op.bind_data, *new_global_state,
			                                           *new_written_file_info->file_stats);
		}
		global_state = std::move(new_global_state);
		written_file_info = std::move(new_written_file_info);
		initialized.store(true, std::memory_order_release);
	}

	void AddBatchData(idx_t batch_index, unique_ptr<PreparedBatchData> new_batch, idx_t memory_usage,
	                  const CopyFunctionBatchAnalyzer &batch_analyzer) {
		// move the batch data to the set of prepared batch data
		lock_guard<mutex> l(lock);
		auto prepared_data = unique_ptr<FixedPreparedBatchData>(
		    new FixedPreparedBatchData {memory_usage, std::move(new_batch), batch_analyzer});
		auto entry = batch_data.insert(make_pair(batch_index, std::move(prepared_data)));
		if (!entry.second) {
			throw InternalException("Duplicate batch index %llu encountered in PhysicalFixedBatchCopy", batch_index);
		}
	}

	idx_t MaxThreads(idx_t source_max_threads) override {
		// try to request 4MB per column per thread
		memory_manager.SetMemorySize(source_max_threads * minimum_memory_per_thread);
		// cap the concurrent threads working on this task based on the amount of available memory
		return MinValue<idx_t>(source_max_threads, memory_manager.AvailableMemory() / minimum_memory_per_thread + 1);
	}
};

enum class FixedBatchCopyState : uint8_t { SINKING_DATA = 1, PROCESSING_TASKS = 2 };

class FixedBatchCopyLocalState : public LocalSinkState {
public:
	explicit FixedBatchCopyLocalState(unique_ptr<LocalFunctionData> local_state_p)
	    : local_state(std::move(local_state_p)), rows_copied(0), local_memory_usage(0) {
	}

	//! Local copy state
	unique_ptr<LocalFunctionData> local_state;
	//! The current collection we are appending to
	unique_ptr<ColumnDataCollection> collection;
	//! The append state of the collection
	ColumnDataAppendState append_state;
	//! How many rows have been copied in total
	idx_t rows_copied;
	//! Memory usage of the thread-local collection
	idx_t local_memory_usage;
	//! The current batch index
	optional_idx batch_index;
	//! Current task
	FixedBatchCopyState current_task = FixedBatchCopyState::SINKING_DATA;

	void InitializeCollection(ClientContext &context, const PhysicalOperator &op) {
		collection = make_uniq<ColumnDataCollection>(context, op.children[0].get().GetTypes());
		collection->SetPartitionIndex(0); // Makes the buffer manager less likely to spill this data
		collection->InitializeAppend(append_state);
		local_memory_usage = 0;
	}
};

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalBatchCopyToFile::Sink(ExecutionContext &context, DataChunk &chunk,
                                             OperatorSinkInput &input) const {
	auto &state = input.local_state.Cast<FixedBatchCopyLocalState>();
	auto &gstate = input.global_state.Cast<FixedBatchCopyGlobalState>();
	auto &memory_manager = gstate.memory_manager;
	auto batch_index = state.partition_info.batch_index.GetIndex();
	if (state.current_task == FixedBatchCopyState::PROCESSING_TASKS) {
		ExecuteTasks(context.client, gstate);
		FlushBatchData(context.client, gstate);

		if (!memory_manager.IsMinimumBatchIndex(batch_index) && memory_manager.OutOfMemory(batch_index)) {
			annotated_lock_guard<annotated_mutex> guard(memory_manager.lock);
			if (!memory_manager.IsMinimumBatchIndex(batch_index)) {
				// no tasks to process, we are not the minimum batch index and we have no memory available to buffer
				// block the task for now
				return memory_manager.BlockSink(input.interrupt_state);
			}
		}
		state.current_task = FixedBatchCopyState::SINKING_DATA;
	}
	if (!memory_manager.IsMinimumBatchIndex(batch_index)) {
		memory_manager.UpdateMinBatchIndex(state.partition_info.min_batch_index.GetIndex());

		// we are not processing the current min batch index
		// check if we have exceeded the maximum number of unflushed rows
		if (memory_manager.OutOfMemory(batch_index)) {
			// out-of-memory - stop sinking chunks and instead assist in processing tasks for the minimum batch index
			state.current_task = FixedBatchCopyState::PROCESSING_TASKS;
			return Sink(context, chunk, input);
		}
	}
	if (!write_empty_file) {
		// if we are not writing empty files - initialize after we have received rows
		gstate.Initialize(context.client, *this);
	}
	if (!state.collection) {
		state.InitializeCollection(context.client, *this);
		state.batch_index = batch_index;
	}
	state.rows_copied += chunk.size();
	state.collection->Append(state.append_state, chunk);
	auto new_memory_usage = state.collection->AllocationSize();
	if (new_memory_usage > state.local_memory_usage) {
		// memory usage increased - add to global state
		memory_manager.IncreaseUnflushedMemory(new_memory_usage - state.local_memory_usage);
		state.local_memory_usage = new_memory_usage;
	} else if (new_memory_usage < state.local_memory_usage) {
		throw InternalException("PhysicalFixedBatchCopy - memory usage decreased somehow?");
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalBatchCopyToFile::Combine(ExecutionContext &context,
                                                       OperatorSinkCombineInput &input) const {
	auto &state = input.local_state.Cast<FixedBatchCopyLocalState>();
	auto &gstate = input.global_state.Cast<FixedBatchCopyGlobalState>();
	auto &memory_manager = gstate.memory_manager;
	gstate.rows_copied += state.rows_copied;

	// add any final remaining local batches
	AddLocalBatch(context.client, gstate, state);

	if (!gstate.any_finished) {
		// signal that this thread is finished processing batches and that we should move on to Finalize
		lock_guard<mutex> l(gstate.lock);
		gstate.any_finished = true;
	}
	memory_manager.UpdateMinBatchIndex(state.partition_info.min_batch_index.GetIndex());
	ExecuteTasks(context.client, gstate);

	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// ProcessRemainingBatchesEvent
//===--------------------------------------------------------------------===//
class ProcessRemainingBatchesTask : public ExecutorTask {
public:
	ProcessRemainingBatchesTask(Executor &executor, shared_ptr<Event> event_p, FixedBatchCopyGlobalState &state_p,
	                            ClientContext &context, const PhysicalBatchCopyToFile &op)
	    : ExecutorTask(executor, std::move(event_p)), op(op), gstate(state_p), context(context) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		while (op.ExecuteTask(context, gstate)) {
			op.FlushBatchData(context, gstate);
		}
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "ProcessRemainingBatchesTask";
	}

private:
	const PhysicalBatchCopyToFile &op;
	FixedBatchCopyGlobalState &gstate;
	ClientContext &context;
};

class ProcessRemainingBatchesEvent : public BasePipelineEvent {
public:
	ProcessRemainingBatchesEvent(const PhysicalBatchCopyToFile &op_p, FixedBatchCopyGlobalState &gstate_p,
	                             Pipeline &pipeline_p, ClientContext &context)
	    : BasePipelineEvent(pipeline_p), op(op_p), gstate(gstate_p), context(context) {
	}
	const PhysicalBatchCopyToFile &op;
	FixedBatchCopyGlobalState &gstate;
	ClientContext &context;

public:
	void Schedule() override {
		vector<shared_ptr<Task>> tasks;
		for (idx_t i = 0; i < TaskScheduler::GetScheduler(context).NumberOfThreads(); i++) {
			auto process_task =
			    make_uniq<ProcessRemainingBatchesTask>(pipeline->executor, shared_from_this(), gstate, context, op);
			tasks.push_back(std::move(process_task));
		}
		D_ASSERT(!tasks.empty());
		SetTasks(std::move(tasks));
	}

	void FinishEvent() override {
		//! Now that all batches are processed we finish flushing the file to disk
		op.FinalFlush(context, gstate);
	}
};
//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType PhysicalBatchCopyToFile::FinalFlush(ClientContext &context, GlobalSinkState &gstate_p) const {
	auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();
	if (gstate.task_manager.TaskCount() != 0) {
		throw InternalException("Unexecuted tasks are remaining in PhysicalFixedBatchCopy::FinalFlush!?");
	}

	FlushBatchData(context, gstate_p);
	if (gstate.scheduled_batch_index != gstate.flushed_batch_index) {
		throw InternalException("Not all batches were flushed to disk - incomplete file?");
	}
	gstate.memory_manager.FinalCheck();
	if (function.copy_to_finalize && gstate.global_state) {
		function.copy_to_finalize(context, *bind_data, *gstate.global_state);
		if (use_tmp_file) {
			PhysicalCopyToFile::MoveTmpFile(context, file_path);
		}
	}
	gstate.output_lifecycle.MarkSuccessful();
	return SinkFinalizeType::READY;
}

SinkFinalizeType PhysicalBatchCopyToFile::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                   OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<FixedBatchCopyGlobalState>();
	auto min_batch_index = idx_t(NumericLimits<int64_t>::Maximum());
	// repartition any remaining batches
	RepartitionBatches(context, input.global_state, min_batch_index, true);
	// check if we have multiple tasks to execute
	if (gstate.task_manager.TaskCount() <= 1) {
		// we don't - just execute the remaining task and finish flushing to disk
		ExecuteTasks(context, input.global_state);
		FinalFlush(context, input.global_state);
	} else {
		// we have multiple tasks remaining - launch an event to execute the tasks in parallel
		auto new_event = make_shared_ptr<ProcessRemainingBatchesEvent>(*this, gstate, pipeline, context);
		event.InsertEvent(std::move(new_event));
	}
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Tasks
//===--------------------------------------------------------------------===//
class RepartitionedFlushTask : public BatchCopyTask {
public:
	RepartitionedFlushTask() {
	}

	void Execute(const PhysicalBatchCopyToFile &op, ClientContext &context, GlobalSinkState &gstate_p) override {
		op.FlushBatchData(context, gstate_p);
	}
};

class PrepareBatchTask : public BatchCopyTask {
public:
	PrepareBatchTask(idx_t batch_index, unique_ptr<FixedRawBatchData> batch_data_p)
	    : batch_index(batch_index), batch_data(std::move(batch_data_p)) {
	}

	idx_t batch_index;
	unique_ptr<FixedRawBatchData> batch_data;

	void Execute(const PhysicalBatchCopyToFile &op, ClientContext &context, GlobalSinkState &gstate_p) override {
		auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();
		auto memory_usage = batch_data->memory_usage;
		const CopyFunctionBatchAnalyzer batch_analyzer(*batch_data->collection, op.batch_size, op.batch_size_bytes);
		auto prepared_batch =
		    op.function.prepare_batch(context, *op.bind_data, *gstate.global_state, std::move(batch_data->collection));
		gstate.AddBatchData(batch_index, std::move(prepared_batch), memory_usage, batch_analyzer);
		if (batch_index == gstate.flushed_batch_index) {
			gstate.task_manager.AddTask(make_uniq<RepartitionedFlushTask>());
		}
	}
};

//===--------------------------------------------------------------------===//
// Batch Data Handling
//===--------------------------------------------------------------------===//
void PhysicalBatchCopyToFile::AddRawBatchData(ClientContext &context, GlobalSinkState &gstate_p, idx_t batch_index,
                                              unique_ptr<FixedRawBatchData> raw_batch) const {
	auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();

	// add the batch index to the set of raw batches
	lock_guard<mutex> l(gstate.lock);
	auto entry = gstate.raw_batches.insert(make_pair(batch_index, std::move(raw_batch)));
	if (!entry.second) {
		throw InternalException("Duplicate batch index %llu encountered in PhysicalFixedBatchCopy", batch_index);
	}
}

static bool CorrectSizeForBatch(const ColumnDataCollection &batch, const optional_idx &batch_size,
                                const optional_idx &batch_size_bytes) {
	if (batch_size.GetIndex() == 0) {
		// a batch size of 0 indicates we are happy with any batch size
		return true;
	}

	const auto size_vector_diff =
	    NumericCast<int64_t>(batch.Count()) - NumericCast<int64_t>(batch_size.GetIndex()) / STANDARD_VECTOR_SIZE;

	int64_t size_bytes_vector_diff = 0;
	if (batch_size_bytes.IsValid()) {
		const auto size_bytes_overshoot =
		    NumericCast<int64_t>(batch.SizeInBytes()) - NumericCast<int64_t>(batch_size_bytes.GetIndex());
		const auto bytes_per_tuple = NumericCast<int64_t>(batch.SizeInBytes() / batch.Count()) + 1;
		size_bytes_vector_diff = size_bytes_overshoot / bytes_per_tuple / STANDARD_VECTOR_SIZE;
	}

	if (size_vector_diff == 0) {
		// The row count diff is within one vector, OK if byte size overshot 2 vectors or less
		return size_bytes_vector_diff <= 2;
	}

	if (size_bytes_vector_diff == 0) {
		// The byte size diff is within two vectors, OK if row count is low
		return size_vector_diff <= 0;
	}

	return false;
}

void PhysicalBatchCopyToFile::RepartitionBatches(ClientContext &context, GlobalSinkState &gstate_p, idx_t min_index,
                                                 bool final) const {
	auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();
	auto &task_manager = gstate.task_manager;

	// repartition batches until the min index is reached
	lock_guard<mutex> l(gstate.lock);
	if (gstate.raw_batches.empty()) {
		return;
	}
	if (!final) {
		if (gstate.any_finished) {
			// we only repartition in ::NextBatch if all threads are still busy processing batches
			// otherwise we might end up repartitioning a lot of data with only a few threads remaining
			// which causes erratic performance
			return;
		}
		// if this is not the final flush we first check if we have enough data to merge past the batch threshold
		idx_t candidate_rows = 0;
		idx_t candidate_size_bytes = 0;
		for (auto entry = gstate.raw_batches.begin(); entry != gstate.raw_batches.end(); entry++) {
			if (entry->first >= min_index) {
				// we have exceeded the minimum batch
				break;
			}
			candidate_rows += entry->second->collection->Count();
			candidate_size_bytes += entry->second->collection->SizeInBytes();
		}
		const CopyFunctionBatchAnalyzer batch_analyzer(candidate_rows, candidate_size_bytes, batch_size,
		                                               batch_size_bytes);
		if (!batch_analyzer.MeetsFlushCriteria()) {
			// not enough rows/bytes - cancel!
			return;
		}
	}
	// gather all collections we can repartition
	idx_t max_batch_index = 0;
	vector<unique_ptr<FixedRawBatchData>> raw_batches;
	for (auto entry = gstate.raw_batches.begin(); entry != gstate.raw_batches.end();) {
		if (entry->first >= min_index) {
			break;
		}
		max_batch_index = entry->first;
		raw_batches.push_back(std::move(entry->second));
		entry = gstate.raw_batches.erase(entry);
	}
	unique_ptr<FixedRawBatchData> append_batch;
	ColumnDataAppendState append_state;
	CopyBatchSlicer batch_slicer(children[0].get().GetTypes(), batch_size_bytes);
	// now perform the actual repartitioning
	for (auto &current_batch : raw_batches) {
		if (!append_batch) {
			const CopyFunctionBatchAnalyzer batch_analyzer(*current_batch->collection, batch_size, batch_size_bytes);
			if (batch_analyzer.IsAcceptable()) {
				// the collection is ~approximately equal to the batch size (off by at most one vector)
				// use it directly
				task_manager.AddTask(
				    make_uniq<PrepareBatchTask>(gstate.scheduled_batch_index++, std::move(current_batch)));
				current_batch.reset();
			} else if (!batch_analyzer.MeetsFlushCriteria()) {
				// the collection is smaller than the batch size - use it as a starting point
				append_batch = std::move(current_batch);
				current_batch.reset();
			} else {
				// the collection is too large for a batch - we need to repartition
				// create an empty collection
				auto new_collection = make_uniq<ColumnDataCollection>(context, children[0].get().GetTypes());
				new_collection->SetPartitionIndex(0); // Makes the buffer manager less likely to spill this data
				append_batch = make_uniq<FixedRawBatchData>(0U, std::move(new_collection));
			}
			if (append_batch) {
				append_batch->collection->InitializeAppend(append_state);
			}
		}
		if (!current_batch) {
			// we have consumed the collection already - no need to append
			continue;
		}
		auto &current_collection = *current_batch->collection;
		append_batch->memory_usage += current_batch->memory_usage;
		// iterate the collection while appending
		for (auto &chunk : current_collection.Chunks()) {
			idx_t offset = 0;
			while (offset < chunk.size()) {
				auto &slice = batch_slicer.Slice(chunk, offset, *append_batch->collection);
				append_batch->collection->Append(append_state, slice);
				const CopyFunctionBatchAnalyzer batch_analyzer(*append_batch->collection, batch_size, batch_size_bytes);
				if (!batch_analyzer.MeetsFlushCriteria()) {
					continue; // the collection is still under the desired batch size
				}
				// the collection is full - move it to the result and create a new one
				task_manager.AddTask(
				    make_uniq<PrepareBatchTask>(gstate.scheduled_batch_index++, std::move(append_batch)));

				auto new_collection = make_uniq<ColumnDataCollection>(context, children[0].get().GetTypes());
				new_collection->SetPartitionIndex(0); // Makes the buffer manager less likely to spill this data
				append_batch = make_uniq<FixedRawBatchData>(0U, std::move(new_collection));
				append_batch->collection->InitializeAppend(append_state);
			}
		}
	}
	if (append_batch && append_batch->collection->Count() > 0) {
		// if there are any remaining batches that are not filled up to the batch size
		// AND this is not the final collection
		// re-add it to the set of raw (to-be-merged) batches
		if (final || CorrectSizeForBatch(*append_batch->collection, batch_size, batch_size_bytes)) {
			task_manager.AddTask(make_uniq<PrepareBatchTask>(gstate.scheduled_batch_index++, std::move(append_batch)));
		} else {
			gstate.raw_batches[max_batch_index] = std::move(append_batch);
		}
	}
}

void PhysicalBatchCopyToFile::FlushBatchData(ClientContext &context, GlobalSinkState &gstate_p) const {
	auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();
	auto &memory_manager = gstate.memory_manager;

	// flush batch data to disk (if there are any to flush)
	// grab the flush lock - we can only call flush_batch with this lock
	// otherwise the data might end up in the wrong order
	{
		lock_guard<mutex> l(gstate.flush_lock);
		if (gstate.any_flushing) {
			return;
		}
		gstate.any_flushing = true;
	}
	ActiveFlushGuard active_flush(gstate.any_flushing);
	while (true) {
		unique_ptr<FixedPreparedBatchData> batch_data;
		{
			lock_guard<mutex> l(gstate.lock);
			if (gstate.batch_data.empty()) {
				// no batch data left to flush
				break;
			}
			auto entry = gstate.batch_data.begin();
			if (entry->first != gstate.flushed_batch_index) {
				// this entry is not yet ready to be flushed
				break;
			}
			if (entry->first < gstate.flushed_batch_index) {
				throw InternalException("Batch index was out of order!?");
			}
			batch_data = std::move(entry->second);
			gstate.batch_data.erase(entry);
		}
		DUCKDB_LOG(context, PhysicalOperatorLogType, *this, "PhysicalBatchCopyToFile", "FlushBatch",
		           {{"file", file_path},
		            {"rows", to_string(batch_data->batch_analyzer.current_batch_size)},
		            {"size", to_string(batch_data->batch_analyzer.current_batch_size_bytes)},
		            {"reason", EnumUtil::ToString(batch_data->batch_analyzer.ToReason())}});
		function.flush_batch(context, *bind_data, *gstate.global_state, *batch_data->prepared_data);
		batch_data->prepared_data.reset();
		memory_manager.ReduceUnflushedMemory(batch_data->memory_usage);
		gstate.flushed_batch_index++;
	}
}

//===--------------------------------------------------------------------===//
// Tasks
//===--------------------------------------------------------------------===//
bool PhysicalBatchCopyToFile::ExecuteTask(ClientContext &context, GlobalSinkState &gstate_p) const {
	auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();
	auto task = gstate.task_manager.GetTask();
	if (!task) {
		return false;
	}
	task->Execute(*this, context, gstate_p);
	return true;
}

void PhysicalBatchCopyToFile::ExecuteTasks(ClientContext &context, GlobalSinkState &gstate_p) const {
	while (ExecuteTask(context, gstate_p)) {
	}
}

//===--------------------------------------------------------------------===//
// Next Batch
//===--------------------------------------------------------------------===//
void PhysicalBatchCopyToFile::AddLocalBatch(ClientContext &context, GlobalSinkState &gstate_p,
                                            LocalSinkState &lstate) const {
	auto &state = lstate.Cast<FixedBatchCopyLocalState>();
	auto &gstate = gstate_p.Cast<FixedBatchCopyGlobalState>();
	auto &memory_manager = gstate.memory_manager;
	if (!state.collection || state.collection->Count() == 0) {
		return;
	}
	// we finished processing this batch
	// start flushing data
	auto min_batch_index = state.partition_info.min_batch_index.GetIndex();
	// push the raw batch data into the set of unprocessed batches
	auto raw_batch = make_uniq<FixedRawBatchData>(state.local_memory_usage, std::move(state.collection));
	AddRawBatchData(context, gstate, state.batch_index.GetIndex(), std::move(raw_batch));
	// attempt to repartition to our desired batch size
	RepartitionBatches(context, gstate, min_batch_index);
	// unblock tasks so they can help process batches (if any are blocked)
	bool any_unblocked;
	{
		annotated_lock_guard<annotated_mutex> guard(memory_manager.lock);
		any_unblocked = memory_manager.UnblockTasks();
	}
	// if any threads were unblocked they can pick up execution of the tasks
	// otherwise we will execute a task and flush here
	if (!any_unblocked) {
		//! Execute a single repartition task
		ExecuteTask(context, gstate);
		//! Flush batch data to disk (if any is ready)
		FlushBatchData(context, gstate);
	}
}

SinkNextBatchType PhysicalBatchCopyToFile::NextBatch(ExecutionContext &context,
                                                     OperatorSinkNextBatchInput &input) const {
	auto &lstate = input.local_state;
	auto &state = lstate.Cast<FixedBatchCopyLocalState>();
	auto &gstate = input.global_state.Cast<FixedBatchCopyGlobalState>();
	auto &memory_manager = gstate.memory_manager;

	// add the previously finished batch (if any) to the state
	AddLocalBatch(context.client, gstate, state);

	// update the minimum batch index
	memory_manager.UpdateMinBatchIndex(state.partition_info.min_batch_index.GetIndex());
	state.batch_index = lstate.partition_info.batch_index.GetIndex();

	state.InitializeCollection(context.client, *this);
	return SinkNextBatchType::READY;
}

SinkNextBatchType PhysicalBatchCopyToFile::UpdateMinBatchIndex(ExecutionContext &,
                                                               OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<FixedBatchCopyGlobalState>();
	gstate.memory_manager.UpdateMinBatchIndex(input.local_state.partition_info.min_batch_index.GetIndex());
	return SinkNextBatchType::READY;
}

unique_ptr<LocalSinkState> PhysicalBatchCopyToFile::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<FixedBatchCopyLocalState>(function.copy_to_initialize_local(context, *bind_data));
}

unique_ptr<GlobalSinkState> PhysicalBatchCopyToFile::GetGlobalSinkState(ClientContext &context) const {
	// request memory based on the minimum amount of memory per column
	auto minimum_memory_per_thread =
	    FixedBatchCopyGlobalState::MINIMUM_MEMORY_PER_COLUMN_PER_THREAD * children[0].get().GetTypes().size();
	auto result = make_uniq<FixedBatchCopyGlobalState>(context, minimum_memory_per_thread);
	if (write_empty_file) {
		// if we are writing the file also if it is empty - initialize now
		result->Initialize(context, *this);
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalBatchCopyToFile::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                          OperatorSourceInput &input) const {
	auto &g = sink_state->Cast<FixedBatchCopyGlobalState>();
	auto fp = use_tmp_file ? PhysicalCopyToFile::GetNonTmpFile(context.client, file_path) : file_path;
	auto return_column_types = CopyFunctionReturnsColumnTypes(return_type);
	switch (GetCopyFunctionBaseReturnType(return_type)) {
	case CopyFunctionReturnType::CHANGED_ROWS:
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(g.rows_copied.load())));
		break;
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST: {
		vector<Value> file_list;
		if (g.global_state) {
			file_list.emplace_back(std::move(fp));
		}
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(g.rows_copied.load())));
		chunk.data[1].Append(Value::LIST(LogicalType::VARCHAR, std::move(file_list)));
		break;
	}
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS: {
		if (g.written_file_info) {
			g.written_file_info->file_path = std::move(fp);
			PhysicalCopyToFile::ReturnStatistics(chunk, *g.written_file_info);
		} else if (return_column_types) {
			// WRITE_EMPTY_FILE false writes no file without rows
			PhysicalCopyToFile::ReturnNoFileStatistics(chunk);
		}
		break;
	}
	default:
		throw NotImplementedException("Unknown CopyFunctionReturnType");
	}

	if (return_column_types) {
		PhysicalCopyToFile::ReturnColumnTypes(chunk, query_names, query_types);
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
