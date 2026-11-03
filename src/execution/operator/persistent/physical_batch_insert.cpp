#include "duckdb/execution/operator/persistent/physical_batch_insert.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/operator/persistent/batch_memory_manager.hpp"
#include "duckdb/execution/operator/persistent/batch_task_manager.hpp"
#include "duckdb/execution/operator/persistent/collection_merger.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

PhysicalBatchInsert::PhysicalBatchInsert(PhysicalPlan &physical_plan, vector<LogicalType> types_p,
                                         DuckTableEntry &table, vector<unique_ptr<BoundConstraint>> bound_constraints_p,
                                         idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::BATCH_INSERT, std::move(types_p), estimated_cardinality),
      insert_table(&table), insert_types(table.GetTypes()), bound_constraints(std::move(bound_constraints_p)),
      preferred_batch_size(table.GetStorage().GetRowGroupSize()) {
}

PhysicalBatchInsert::PhysicalBatchInsert(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
                                         unique_ptr<BoundCreateTableInfo> info_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::BATCH_CREATE_TABLE_AS, op.types, estimated_cardinality),
      insert_table(nullptr), schema(&schema), info(std::move(info_p)), preferred_batch_size(DEFAULT_ROW_GROUP_SIZE) {
	PhysicalInsert::GetInsertInfo(*info, insert_types);
}

OperatorPartitionInfo PhysicalBatchInsert::RequiredPartitionInfo() const {
	return OperatorPartitionInfo::BatchIndex(preferred_batch_size);
}

struct RowGroupBatchEntry {
	RowGroupBatchEntry(OptimisticWriteCollection &collection, const idx_t batch_idx,
	                   const PhysicalIndex collection_index, const RowGroupBatchType type)
	    : batch_idx(batch_idx), total_rows(collection.collection->GetTotalRows()), unflushed_memory(0),
	      collection_index(collection_index), type(type) {
		if (type == RowGroupBatchType::NOT_FLUSHED) {
			unflushed_memory = collection.collection->GetAllocationSize();
		}
	}

	idx_t batch_idx;
	idx_t total_rows;
	idx_t unflushed_memory;
	PhysicalIndex collection_index;
	RowGroupBatchType type;
};

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class BatchInsertTask {
public:
	virtual ~BatchInsertTask() {
	}

	virtual void Execute(const PhysicalBatchInsert &op, ClientContext &context, GlobalSinkState &gstate_p,
	                     LocalSinkState &lstate_p) = 0;
};

class BatchInsertGlobalState : public GlobalSinkState {
public:
	BatchInsertGlobalState(ClientContext &context, DuckTableEntry &table, const idx_t minimum_memory_per_thread)
	    : memory_manager(context, minimum_memory_per_thread), table(table), insert_count(0),
	      optimistically_written(false), minimum_memory_per_thread(minimum_memory_per_thread) {
		row_group_size = table.GetStorage().GetRowGroupSize();
		table.GetStorage().BindIndexes(context);
	}

	BatchMemoryManager memory_manager;
	BatchTaskManager<BatchInsertTask> task_manager;
	annotated_mutex lock;
	DuckTableEntry &table;
	idx_t row_group_size;
	idx_t insert_count;
	vector<RowGroupBatchEntry> collections;
	idx_t next_start = 0;
	atomic<bool> optimistically_written;
	idx_t minimum_memory_per_thread;

	bool ReadyToMerge(const idx_t count) const;
	void ScheduleMergeTasks(ClientContext &context, const idx_t min_batch_index);
	PhysicalIndex MergeCollections(ClientContext &context, const vector<RowGroupBatchEntry> &merge_collections,
	                               OptimisticDataWriter &writer);
	void AddCollection(ClientContext &context, const idx_t batch_index, const idx_t min_batch_index,
	                   const PhysicalIndex collection_index, optional_ptr<OptimisticDataWriter> writer = nullptr);

	idx_t MaxThreads(const idx_t source_max_threads) override {
		// try to request 4MB per column per thread
		memory_manager.SetMemorySize(source_max_threads * minimum_memory_per_thread);
		// cap the concurrent threads working on this task based on the amount of available memory
		return MinValue<idx_t>(source_max_threads, memory_manager.AvailableMemory() / minimum_memory_per_thread + 1);
	}
};

class BatchInsertLocalState : public LocalSinkState {
public:
	BatchInsertLocalState(ClientContext &context, const vector<LogicalType> &types)
	    : collection_index(DConstants::INVALID_INDEX) {
	}

	idx_t current_index = 0;
	TableAppendState current_append_state;
	PhysicalIndex collection_index;
	unique_ptr<OptimisticDataWriter> optimistic_writer;
	unique_ptr<ConstraintState> constraint_state;

	void CreateNewCollection(ClientContext &context, DuckTableEntry &table_entry,
	                         const vector<LogicalType> &insert_types) {
		if (!optimistic_writer) {
			optimistic_writer = make_uniq<OptimisticDataWriter>(context, table_entry.GetStorage());
		}
		auto collection = optimistic_writer->CreateCollection(table_entry.GetStorage(), insert_types);
		auto &row_collection = *collection->collection;
		row_collection.InitializeEmpty();
		row_collection.InitializeAppend(current_append_state);

		auto &data_table = table_entry.GetStorage();
		collection_index = data_table.CreateOptimisticCollection(context, std::move(collection));
	}
};

//===--------------------------------------------------------------------===//
// Merge Task
//===--------------------------------------------------------------------===//
class MergeCollectionTask : public BatchInsertTask {
public:
	MergeCollectionTask(vector<RowGroupBatchEntry> merge_collections_p, idx_t merged_batch_index)
	    : merge_collections(std::move(merge_collections_p)), merged_batch_index(merged_batch_index) {
	}

	vector<RowGroupBatchEntry> merge_collections;
	idx_t merged_batch_index;

	void Execute(const PhysicalBatchInsert &op, ClientContext &context, GlobalSinkState &g_state_p,
	             LocalSinkState &l_state_p) override {
		auto &g_state = g_state_p.Cast<BatchInsertGlobalState>();
		auto &l_state = l_state_p.Cast<BatchInsertLocalState>();

		// Merge the collections.
		if (!l_state.optimistic_writer) {
			l_state.optimistic_writer = make_uniq<OptimisticDataWriter>(context, g_state.table.GetStorage());
		}
		auto result_collection_index = g_state.MergeCollections(context, merge_collections, *l_state.optimistic_writer);
		merge_collections.clear();

		annotated_lock_guard<annotated_mutex> l(g_state.lock);
		auto &result_collection = g_state.table.GetStorage().GetOptimisticCollection(context, result_collection_index);
		RowGroupBatchEntry new_entry(result_collection, merged_batch_index, result_collection_index,
		                             RowGroupBatchType::FLUSHED);

		// Add the result collection to the set of batch indexes.
		auto it = std::lower_bound(
		    g_state.collections.begin(), g_state.collections.end(), new_entry,
		    [&](const RowGroupBatchEntry &a, const RowGroupBatchEntry &b) { return a.batch_idx < b.batch_idx; });
		if (it->batch_idx != merged_batch_index) {
			throw InternalException("Merged batch index was no longer present in collection");
		}
		it->collection_index = new_entry.collection_index;
	}
};

struct BatchMergeTask {
	explicit BatchMergeTask(idx_t start_index) : start_index(start_index), end_index(0), total_count(0) {
	}

	idx_t start_index;
	idx_t end_index;
	idx_t total_count;
};

bool BatchInsertGlobalState::ReadyToMerge(const idx_t count) const {
	// we try to merge so the count fits nicely into row groups
	if (count >= row_group_size / 10 * 9 && count <= row_group_size) {
		// 90%-100% of row group size
		return true;
	}
	if (count >= row_group_size / 10 * 18 && count <= row_group_size * 2) {
		// 180%-200% of row group size
		return true;
	}
	if (count >= row_group_size / 10 * 27 && count <= row_group_size * 3) {
		// 270%-300% of row group size
		return true;
	}
	if (count >= row_group_size / 10 * 36) {
		// >360% of row group size
		return true;
	}
	return false;
}

void BatchInsertGlobalState::ScheduleMergeTasks(ClientContext &context, const idx_t min_batch_index) {
	idx_t current_idx;
	vector<BatchMergeTask> to_be_scheduled_tasks;

	BatchMergeTask current_task(next_start);
	for (current_idx = current_task.start_index; current_idx < collections.size(); current_idx++) {
		auto &entry = collections[current_idx];
		if (entry.batch_idx > min_batch_index) {
			// this entry is AFTER the min_batch_index
			// finished
			if (ReadyToMerge(current_task.total_count)) {
				current_task.end_index = current_idx;
				to_be_scheduled_tasks.push_back(current_task);
			}
			break;
		}
		if (entry.type == RowGroupBatchType::FLUSHED) {
			// already flushed: cannot flush anything here
			if (current_task.total_count > 0) {
				current_task.end_index = current_idx;
				to_be_scheduled_tasks.push_back(current_task);
			}
			current_task.start_index = current_idx + 1;
			if (current_task.start_index > next_start) {
				// avoid checking this segment again in the future
				next_start = current_task.start_index;
			}
			current_task.total_count = 0;
			continue;
		}
		// not flushed - add to set of indexes to flush
		current_task.total_count += entry.total_rows;
		if (ReadyToMerge(current_task.total_count)) {
			// create a task to merge these collections
			current_task.end_index = current_idx + 1;
			to_be_scheduled_tasks.push_back(current_task);
			current_task.start_index = current_idx + 1;
			current_task.total_count = 0;
		}
	}

	if (to_be_scheduled_tasks.empty()) {
		return;
	}
	for (auto &scheduled_task : to_be_scheduled_tasks) {
		D_ASSERT(scheduled_task.total_count > 0);
		D_ASSERT(current_idx > scheduled_task.start_index);
		auto merged_batch_index = collections[scheduled_task.start_index].batch_idx;
		vector<RowGroupBatchEntry> merge_collections;
		for (idx_t idx = scheduled_task.start_index; idx < scheduled_task.end_index; idx++) {
			auto &entry = collections[idx];
			if (!entry.collection_index.IsValid() || entry.type == RowGroupBatchType::FLUSHED) {
				throw InternalException("Adding a row group collection that should not be flushed");
			}
			auto &collection = table.GetStorage().GetOptimisticCollection(context, entry.collection_index);
			RowGroupBatchEntry added_entry(collection, collections[scheduled_task.start_index].batch_idx,
			                               entry.collection_index, RowGroupBatchType::FLUSHED);
			added_entry.unflushed_memory = entry.unflushed_memory;
			merge_collections.push_back(added_entry);
			entry.total_rows = scheduled_task.total_count;
			entry.type = RowGroupBatchType::FLUSHED;
			entry.collection_index = PhysicalIndex(DConstants::INVALID_INDEX);
		}
		task_manager.AddTask(make_uniq<MergeCollectionTask>(std::move(merge_collections), merged_batch_index));
	}
	// erase in reverse order
	for (idx_t i = to_be_scheduled_tasks.size(); i > 0; i--) {
		auto &scheduled_task = to_be_scheduled_tasks[i - 1];
		if (scheduled_task.start_index + 1 < scheduled_task.end_index) {
			// erase all entries except the first one
			collections.erase(collections.begin() + NumericCast<int64_t>(scheduled_task.start_index) + 1,
			                  collections.begin() + NumericCast<int64_t>(scheduled_task.end_index));
		}
	}
}

PhysicalIndex BatchInsertGlobalState::MergeCollections(ClientContext &context,
                                                       const vector<RowGroupBatchEntry> &merge_collections,
                                                       OptimisticDataWriter &writer) {
	D_ASSERT(!merge_collections.empty());
	CollectionMerger merger(context, table.GetStorage());
	idx_t written_data = 0;
	for (auto &entry : merge_collections) {
		merger.AddCollection(entry.collection_index, RowGroupBatchType::NOT_FLUSHED);
		written_data += entry.unflushed_memory;
	}
	optimistically_written = true;
	memory_manager.ReduceUnflushedMemory(written_data);
	return merger.Flush(writer);
}

void BatchInsertGlobalState::AddCollection(ClientContext &context, const idx_t batch_index, const idx_t min_batch_index,
                                           const PhysicalIndex collection_index,
                                           optional_ptr<OptimisticDataWriter> writer) {
	if (batch_index < min_batch_index) {
		throw InternalException("Batch index of the added collection (%llu) is smaller than the min batch index (%llu)",
		                        batch_index, min_batch_index);
	}
	auto &optimistic_collection = table.GetStorage().GetOptimisticCollection(context, collection_index);
	auto &collection = *optimistic_collection.collection;
	auto new_count = collection.GetTotalRows();
	auto batch_type = new_count < row_group_size ? RowGroupBatchType::NOT_FLUSHED : RowGroupBatchType::FLUSHED;
	if (batch_type == RowGroupBatchType::FLUSHED && writer) {
		writer->WriteUnflushedRowGroups(optimistic_collection);
	}
	annotated_lock_guard<annotated_mutex> l(lock);
	insert_count += new_count;
	// add the collection to the batch index
	RowGroupBatchEntry new_entry(optimistic_collection, batch_index, collection_index, batch_type);
	if (batch_type == RowGroupBatchType::NOT_FLUSHED) {
		memory_manager.IncreaseUnflushedMemory(new_entry.unflushed_memory);
	}

	auto it = std::lower_bound(
	    collections.begin(), collections.end(), new_entry,
	    [&](const RowGroupBatchEntry &a, const RowGroupBatchEntry &b) { return a.batch_idx < b.batch_idx; });
	if (it != collections.end() && it->batch_idx == new_entry.batch_idx) {
		throw InternalException("PhysicalBatchInsert::AddCollection error: batch index %d is present in multiple "
		                        "collections. This occurs when "
		                        "batch indexes are not uniquely distributed over threads",
		                        batch_index);
	}
	collections.insert(it, new_entry);
	if (writer) {
		ScheduleMergeTasks(context, min_batch_index);
	}
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSinkState> PhysicalBatchInsert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<DuckTableEntry> table;
	if (info) {
		// CREATE TABLE AS
		D_ASSERT(!insert_table);
		auto &catalog = schema->catalog;
		auto created_table = catalog.CreateTable(catalog.GetCatalogTransaction(context), *schema, *info);
		table = &created_table->Cast<DuckTableEntry>();
	} else {
		D_ASSERT(insert_table);
		D_ASSERT(insert_table->IsDuckTable());
		table = insert_table;
	}
	// heuristic - we start off by allocating 4MB of cache space per column
	static constexpr const idx_t MINIMUM_MEMORY_PER_COLUMN = 4ULL * 1024ULL * 1024ULL;
	auto minimum_memory_per_thread = table->GetColumns().PhysicalColumnCount() * MINIMUM_MEMORY_PER_COLUMN;
	auto result = make_uniq<BatchInsertGlobalState>(context, *table, minimum_memory_per_thread);
	return std::move(result);
}

unique_ptr<LocalSinkState> PhysicalBatchInsert::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<BatchInsertLocalState>(context.client, insert_types);
}

//===--------------------------------------------------------------------===//
// Tasks
//===--------------------------------------------------------------------===//
bool PhysicalBatchInsert::ExecuteTask(ClientContext &context, GlobalSinkState &gstate_p,
                                      LocalSinkState &lstate_p) const {
	auto &gstate = gstate_p.Cast<BatchInsertGlobalState>();
	auto task = gstate.task_manager.GetTask();
	if (!task) {
		return false;
	}
	task->Execute(*this, context, gstate_p, lstate_p);
	return true;
}

void PhysicalBatchInsert::ExecuteTasks(ClientContext &context, GlobalSinkState &gstate_p,
                                       LocalSinkState &lstate_p) const {
	while (ExecuteTask(context, gstate_p, lstate_p)) {
	}
}

//===--------------------------------------------------------------------===//
// NextBatch
//===--------------------------------------------------------------------===//
SinkNextBatchType PhysicalBatchInsert::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<BatchInsertGlobalState>();
	auto &lstate = input.local_state.Cast<BatchInsertLocalState>();
	auto &memory_manager = gstate.memory_manager;

	auto batch_index = lstate.partition_info.batch_index.GetIndex();
	if (lstate.collection_index.IsValid()) {
		if (lstate.current_index == batch_index) {
			throw InternalException("NextBatch called with the same batch index?");
		}
		// batch index has changed: move the old collection to the global state and create a new collection
		// the transaction id keeps the rows invisible to other transactions until the commit
		TransactionData tdata(DuckTransaction::Get(context.client, gstate.table.catalog));
		auto &optimistic_collection =
		    gstate.table.GetStorage().GetOptimisticCollection(context.client, lstate.collection_index);
		auto &collection = *optimistic_collection.collection;
		collection.FinalizeAppend(tdata, lstate.current_append_state);
		gstate.AddCollection(context.client, lstate.current_index, lstate.partition_info.min_batch_index.GetIndex(),
		                     lstate.collection_index, lstate.optimistic_writer);

		bool any_unblocked;
		{
			const annotated_lock_guard<annotated_mutex> guard {memory_manager.lock};
			any_unblocked = memory_manager.UnblockTasks();
		}
		if (!any_unblocked) {
			ExecuteTasks(context.client, gstate, lstate);
		}
		lstate.collection_index.index = DConstants::INVALID_INDEX;
	}
	lstate.current_index = batch_index;

	// unblock any blocked tasks
	const annotated_lock_guard<annotated_mutex> guard {memory_manager.lock};
	memory_manager.UnblockTasks();

	return SinkNextBatchType::READY;
}

SinkNextBatchType PhysicalBatchInsert::UpdateMinBatchIndex(ExecutionContext &,
                                                           OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<BatchInsertGlobalState>();
	gstate.memory_manager.UpdateMinBatchIndex(input.local_state.partition_info.min_batch_index.GetIndex());
	return SinkNextBatchType::READY;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalBatchInsert::Sink(ExecutionContext &context, DataChunk &insert_chunk,
                                         OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<BatchInsertGlobalState>();
	auto &lstate = input.local_state.Cast<BatchInsertLocalState>();
	auto &memory_manager = gstate.memory_manager;

	auto &table = gstate.table;
	insert_chunk.Flatten();

	auto batch_index = lstate.partition_info.batch_index.GetIndex();
	// check if we should process this batch
	if (!memory_manager.IsMinimumBatchIndex(batch_index)) {
		memory_manager.UpdateMinBatchIndex(lstate.partition_info.min_batch_index.GetIndex());

		// we are not processing the current min batch index
		// check if we have exceeded the maximum number of unflushed rows
		if (memory_manager.OutOfMemory(batch_index)) {
			// out-of-memory
			// execute tasks while we wait (if any are available)
			ExecuteTasks(context.client, gstate, lstate);

			const annotated_lock_guard<annotated_mutex> guard {memory_manager.lock};
			if (!memory_manager.IsMinimumBatchIndex(batch_index)) {
				//  we are not the minimum batch index and we have no memory available to buffer - block the task for
				//  now
				return memory_manager.BlockSink(input.interrupt_state);
			}
		}
	}
	if (!lstate.collection_index.IsValid()) {
		annotated_lock_guard<annotated_mutex> l(gstate.lock);
		// no collection yet: create a new one
		lstate.CreateNewCollection(context.client, table, insert_types);
	}

	if (lstate.current_index != batch_index) {
		throw InternalException("Current batch differs from batch - but NextBatch was not called!?");
	}

	if (!lstate.constraint_state) {
		lstate.constraint_state = table.GetStorage().InitializeConstraintState(table, bound_constraints);
	}

	auto &storage = table.GetStorage();
	auto &local_storage = LocalStorage::Get(context.client, storage.db);
	auto local_table_storage = local_storage.GetStorage(table.GetStorage());
	storage.VerifyAppendConstraints(*lstate.constraint_state, context.client, insert_chunk, local_table_storage,
	                                nullptr);

	auto &optimistic_collection = table.GetStorage().GetOptimisticCollection(context.client, lstate.collection_index);
	auto &collection = *optimistic_collection.collection;
	auto flushed_row_group_idx = collection.Append(insert_chunk, lstate.current_append_state);
	if (flushed_row_group_idx.IsValid()) {
		// we have already written to disk - flush the next row group as well
		lstate.optimistic_writer->WriteNewRowGroup(optimistic_collection, flushed_row_group_idx.GetIndex());
	}
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
SinkCombineResultType PhysicalBatchInsert::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<BatchInsertGlobalState>();
	auto &lstate = input.local_state.Cast<BatchInsertLocalState>();
	auto &memory_manager = gstate.memory_manager;
	auto &client_profiler = QueryProfiler::Get(context.client);
	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);

	memory_manager.UpdateMinBatchIndex(lstate.partition_info.min_batch_index.GetIndex());

	if (lstate.collection_index.IsValid()) {
		TransactionData tdata(DuckTransaction::Get(context.client, gstate.table.catalog));
		auto &optimistic_collection =
		    gstate.table.GetStorage().GetOptimisticCollection(context.client, lstate.collection_index);
		auto &collection = *optimistic_collection.collection;
		collection.FinalizeAppend(tdata, lstate.current_append_state);
		if (collection.GetTotalRows() > 0) {
			auto batch_index = lstate.partition_info.min_batch_index.GetIndex();
			gstate.AddCollection(context.client, lstate.current_index, batch_index, lstate.collection_index);
			lstate.collection_index = PhysicalIndex(DConstants::INVALID_INDEX);
		}
	}
	if (lstate.optimistic_writer) {
		annotated_lock_guard<annotated_mutex> l(gstate.lock);
		auto &optimistic_writer = gstate.table.GetStorage().GetOptimisticWriter(context.client);
		optimistic_writer.Merge(*lstate.optimistic_writer);
	}

	// unblock any blocked tasks
	const annotated_lock_guard<annotated_mutex> guard {memory_manager.lock};
	memory_manager.UnblockTasks();

	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType PhysicalBatchInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               OperatorSinkFinalizeInput &input) const {
	auto &g_state = input.global_state.Cast<BatchInsertGlobalState>();
	auto &table = g_state.table;
	auto &data_table = g_state.table.GetStorage();
	auto &memory_manager = g_state.memory_manager;

	if (g_state.optimistically_written || g_state.insert_count >= g_state.row_group_size) {
		// we have written data to disk optimistically or are inserting a large amount of data
		// perform a final pass over all of the row groups and merge them together
		vector<unique_ptr<CollectionMerger>> mergers;
		unique_ptr<CollectionMerger> current_merger;

		for (auto &entry : g_state.collections) {
			if (entry.type == RowGroupBatchType::NOT_FLUSHED) {
				// this collection has not been flushed: add it to the merge set
				if (!current_merger) {
					current_merger = make_uniq<CollectionMerger>(context, data_table);
				}
				current_merger->AddCollection(entry.collection_index, entry.type);
				memory_manager.ReduceUnflushedMemory(entry.unflushed_memory);
				continue;
			}

			// This collection has been flushed, so it does not need to be merged.
			// Create a separate collection merger for it.
			if (current_merger) {
				// Flush any remaining small allocations.
				mergers.push_back(std::move(current_merger));
				current_merger.reset();
			}
			auto larger_merger = make_uniq<CollectionMerger>(context, data_table);
			larger_merger->AddCollection(entry.collection_index, entry.type);
			mergers.push_back(std::move(larger_merger));
		}

		g_state.collections.clear();
		if (current_merger) {
			mergers.push_back(std::move(current_merger));
		}

		// now that we have created all of the mergers, perform the actual merging
		vector<PhysicalIndex> final_collections;
		final_collections.reserve(mergers.size());
		auto writer = make_uniq<OptimisticDataWriter>(context, data_table);
		for (auto &merger : mergers) {
			final_collections.push_back(merger->Flush(*writer));
		}

		// finally, merge the row groups into the local storage
		for (const auto collection_index : final_collections) {
			auto &collection = data_table.GetOptimisticCollection(context, collection_index);
			data_table.LocalMerge(context, table, collection);
			data_table.ResetOptimisticCollection(context, collection_index);
		}

		auto &optimistic_writer = data_table.GetOptimisticWriter(context);
		optimistic_writer.Merge(*writer);
		optimistic_writer.FinalFlush();
		memory_manager.FinalCheck();
		return SinkFinalizeType::READY;
	}

	// We are writing a small amount of data to disk.
	// Thus, we append directly to the transaction local storage.
	LocalAppendState append_state;
	data_table.InitializeLocalAppend(append_state, table, context, bound_constraints);
	auto &transaction = DuckTransaction::Get(context, table.catalog);
	for (auto &entry : g_state.collections) {
		if (entry.type != RowGroupBatchType::NOT_FLUSHED) {
			throw InternalException("Encountered a flushed batch");
		}

		memory_manager.ReduceUnflushedMemory(entry.unflushed_memory);
		auto &optimistic_collection = data_table.GetOptimisticCollection(context, entry.collection_index);
		auto &collection = *optimistic_collection.collection;
		for (auto &insert_chunk : collection.Chunks(transaction)) {
			data_table.LocalAppend(append_state, table, context, insert_chunk, false);
		}
		data_table.ResetOptimisticCollection(context, entry.collection_index);
	}

	g_state.collections.clear();
	data_table.FinalizeLocalAppend(append_state);
	memory_manager.FinalCheck();
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//

SourceResultType PhysicalBatchInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                      OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<BatchInsertGlobalState>();

	chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(insert_gstate.insert_count)));

	return SourceResultType::FINISHED;
}

} // namespace duckdb
