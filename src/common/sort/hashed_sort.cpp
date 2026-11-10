#include "duckdb/common/sorting/hashed_sort.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/sorting/partition_key_tracker.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/types/row/integer_partitioned_tuple_data.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// HashedSortGroup
//===--------------------------------------------------------------------===//
class HashedSortGroup {
public:
	using Orders = vector<BoundOrderByNode>;
	using Types = vector<LogicalType>;

	HashedSortGroup(ClientContext &client, Sort &sort, idx_t group_idx);

	const idx_t group_idx;
	atomic<idx_t> count;

	//	Sink
	Sort &sort;
	unique_ptr<GlobalSinkState> sort_global;

	//	Source
	mutex scan_lock;
	TupleDataParallelScanState parallel_scan;
	atomic<idx_t> tasks_completed;
	unique_ptr<GlobalSourceState> sort_source;
	//! Single-key groups skip the sort: their rows are appended here directly (guarded by scan_lock)
	unique_ptr<ColumnDataCollection> direct_column_data;
	ColumnDataAppendState direct_append_state;
};

HashedSortGroup::HashedSortGroup(ClientContext &client, Sort &sort, idx_t group_idx)
    : group_idx(group_idx), count(0), sort(sort), tasks_completed(0) {
	sort_global = sort.GetGlobalSinkState(client);
}

//===--------------------------------------------------------------------===//
// HashedSortGlobalSinkState
//===--------------------------------------------------------------------===//
class HashedSortGlobalSinkState : public GlobalSinkState {
public:
	using HashGroupPtr = unique_ptr<HashedSortGroup>;
	using Orders = vector<BoundOrderByNode>;
	using Types = vector<LogicalType>;

	using GroupingPartition = unique_ptr<PartitionedTupleData>;
	using GroupingAppend = unique_ptr<PartitionedTupleDataAppendState>;

	HashedSortGlobalSinkState(ClientContext &client, const HashedSort &hashed_sort);

	// OVER(PARTITION BY...) (hash grouping)
	unique_ptr<PartitionedTupleData> CreatePartition(idx_t new_bits) const;
	unique_ptr<RepartitionKeyTracker> CreateRepartitionKeyTracker(PartitionKeyTracker &tracker) const;
	void SyncPartitioning(const HashedSortGlobalSinkState &other);
	void UpdateLocalPartition(GroupingPartition &local_partition, GroupingAppend &partition_append,
	                          optional_ptr<PartitionKeyTracker> local_tracker);
	void CombineLocalPartition(GroupingPartition &local_partition, GroupingAppend &local_append,
	                           optional_ptr<PartitionKeyTracker> local_tracker);
	ProgressData GetSinkProgress(ClientContext &context, const ProgressData source_progress) const;
	bool CanBypassSort(idx_t hash_bin) const;

	//! System and query state
	ClientContext &client;
	const HashedSort &hashed_sort;
	BufferManager &buffer_manager;
	Allocator &allocator;
	mutable mutex lock;

	// OVER(PARTITION BY...) (hash grouping)
	GroupingPartition grouping_data;
	//! The number of radix bits of grouping_data (unused when integer partitioning)
	idx_t radix_bits = 0;
	//! Payload plus hash column
	shared_ptr<TupleDataLayout> grouping_types_ptr;
	unique_ptr<PartitionKeyTracker> single_key_tracker;
	//! The number of radix bits if this partition is being synced with another
	idx_t fixed_bits;

	// OVER(...) (sorting)
	vector<HashGroupPtr> hash_groups;

	// Threading
	idx_t max_bits;
	atomic<idx_t> count;

private:
	void Rehash(idx_t cardinality);
	void SyncLocalPartition(GroupingPartition &local_partition, GroupingAppend &local_append,
	                        optional_ptr<PartitionKeyTracker> local_tracker);
};

HashedSortGlobalSinkState::HashedSortGlobalSinkState(ClientContext &client, const HashedSort &hashed_sort)
    : client(client), hashed_sort(hashed_sort), buffer_manager(BufferManager::GetBufferManager(client)),
      allocator(Allocator::Get(client)), fixed_bits(0), max_bits(1), count(0) {
	if (hashed_sort.track_single_keys) {
		single_key_tracker = make_uniq<PartitionKeyTracker>(allocator, hashed_sort.partition_key_types);
	}

	const auto memory_per_thread = PhysicalOperator::GetMaxThreadMemory(client);
	const auto thread_pages = PreviousPowerOfTwo(memory_per_thread / (4 * buffer_manager.GetBlockAllocSize()));
	while (max_bits < 8 && (thread_pages >> max_bits) > 1) {
		++max_bits;
	}

	grouping_types_ptr = make_shared_ptr<TupleDataLayout>();
	auto &payload_types = hashed_sort.payload_types;
	auto types = payload_types;
	types.push_back(LogicalType::HASH);
	grouping_types_ptr->Initialize(types, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	Rehash(hashed_sort.estimated_cardinality);
}

unique_ptr<PartitionedTupleData> HashedSortGlobalSinkState::CreatePartition(idx_t new_bits) const {
	auto &payload_types = hashed_sort.payload_types;
	if (hashed_sort.integer_partitioning) {
		return make_uniq<IntegerPartitionedTupleData>(
		    buffer_manager, grouping_types_ptr, MemoryTag::WINDOW, hashed_sort.partition_key_ids[0],
		    hashed_sort.integer_partition_min, hashed_sort.integer_partition_count, client);
	}
	const auto hash_col_idx = payload_types.size();
	return make_uniq<RadixPartitionedTupleData>(buffer_manager, grouping_types_ptr, MemoryTag::WINDOW, new_bits,
	                                            hash_col_idx, client);
}

unique_ptr<RepartitionKeyTracker>
HashedSortGlobalSinkState::CreateRepartitionKeyTracker(PartitionKeyTracker &tracker) const {
	return make_uniq<RepartitionKeyTracker>(allocator, tracker, hashed_sort.partition_key_types,
	                                        hashed_sort.partition_key_ids);
}

void HashedSortGlobalSinkState::Rehash(idx_t cardinality) {
	//	Integer partitioning is fixed by the key range
	if (hashed_sort.integer_partitioning) {
		if (!grouping_data) {
			grouping_data = CreatePartition(0);
		}
		return;
	}
	//	Have we started to combine? Then just live with it.
	if (fixed_bits) {
		return;
	}
	//	Is the average partition size too large?
	const idx_t partition_size = DEFAULT_ROW_GROUP_SIZE;
	const auto bits = grouping_data ? radix_bits : 0;
	auto new_bits = bits ? bits : 4;
	while (new_bits < max_bits && (cardinality / RadixPartitioning::NumberOfPartitions(new_bits)) > partition_size) {
		new_bits = MinValue(new_bits + 2, max_bits);
	}

	// Repartition the grouping data
	if (new_bits != bits) {
		grouping_data = CreatePartition(new_bits);
		radix_bits = new_bits;
		if (single_key_tracker) {
			single_key_tracker->Reset(new_bits);
		}
	}
}

static idx_t LocalRadixBits(const PartitionedTupleData &local_partition) {
	return local_partition.Cast<RadixPartitionedTupleData>().GetRadixBits();
}

void HashedSortGlobalSinkState::SyncLocalPartition(GroupingPartition &local_partition, GroupingAppend &local_append,
                                                   optional_ptr<PartitionKeyTracker> local_tracker) {
	if (hashed_sort.integer_partitioning) {
		return;
	}
	// We are done if the local_partition is right sized.
	const auto new_bits = radix_bits;
	if (LocalRadixBits(*local_partition) == new_bits) {
		return;
	}

	// If the local partition is now too small, flush it and reallocate
	auto new_partition = CreatePartition(new_bits);
	local_partition->FlushAppendState(*local_append);
	unique_ptr<RepartitionKeyTracker> key_tracker;
	if (local_tracker) {
		local_tracker->Reset(new_bits);
		key_tracker = CreateRepartitionKeyTracker(*local_tracker);
	}
	local_partition->Repartition(client, *new_partition, key_tracker.get());

	local_partition = std::move(new_partition);
	local_append = make_uniq<PartitionedTupleDataAppendState>();
	local_partition->InitializeAppendState(*local_append);
}

void HashedSortGlobalSinkState::UpdateLocalPartition(GroupingPartition &local_partition,
                                                     GroupingAppend &partition_append,
                                                     optional_ptr<PartitionKeyTracker> local_tracker) {
	// First call: initialize the local partition
	if (!local_partition) {
		lock_guard<mutex> guard(lock);
		local_partition = CreatePartition(radix_bits);
		partition_append = make_uniq<PartitionedTupleDataAppendState>();
		local_partition->InitializeAppendState(*partition_append);
		if (local_tracker) {
			local_tracker->Reset(radix_bits);
		}
		return;
	}

	//	Integer partitioning never changes shape
	if (hashed_sort.integer_partitioning) {
		return;
	}

	// Check bits under lock, repartition outside lock, then check again
	while (true) {
		idx_t new_bits;
		{
			lock_guard<mutex> guard(lock);
			Rehash(count);
			new_bits = radix_bits;
		}

		if (LocalRadixBits(*local_partition) == new_bits) {
			return; // Already in sync
		}

		auto new_partition = CreatePartition(new_bits);
		local_partition->FlushAppendState(*partition_append);
		unique_ptr<RepartitionKeyTracker> key_tracker;
		if (local_tracker) {
			local_tracker->Reset(new_bits);
			key_tracker = CreateRepartitionKeyTracker(*local_tracker);
		}
		local_partition->Repartition(client, *new_partition, key_tracker.get());
		local_partition = std::move(new_partition);
		partition_append = make_uniq<PartitionedTupleDataAppendState>();
		local_partition->InitializeAppendState(*partition_append);
	}
}

void HashedSortGlobalSinkState::SyncPartitioning(const HashedSortGlobalSinkState &other) {
	if (hashed_sort.integer_partitioning) {
		return;
	}
	fixed_bits = other.grouping_data ? other.radix_bits : 0;

	const auto old_bits = grouping_data ? radix_bits : 0;
	if (fixed_bits != old_bits) {
		grouping_data = CreatePartition(fixed_bits);
		radix_bits = fixed_bits;
		if (single_key_tracker) {
			single_key_tracker->Reset(fixed_bits);
		}
	}
}

void HashedSortGlobalSinkState::CombineLocalPartition(GroupingPartition &local_partition, GroupingAppend &local_append,
                                                      optional_ptr<PartitionKeyTracker> local_tracker) {
	if (!local_partition) {
		return;
	}
	local_partition->FlushAppendState(*local_append);

	// Make sure grouping_data doesn't change under us.
	// Combine has an internal mutex, so this is single-threaded anyway.
	lock_guard<mutex> guard(lock);
	SyncLocalPartition(local_partition, local_append, local_tracker);
	fixed_bits = true;

	//	We now know the number of hash_groups (some may be empty)
	auto &groups = local_partition->GetPartitions();
	if (hash_groups.empty()) {
		hash_groups.resize(groups.size());
	}

	//	Create missing HashedSortGroups inside the mutex
	for (idx_t group_idx = 0; group_idx < groups.size(); ++group_idx) {
		auto &hash_group = hash_groups[group_idx];
		if (hash_group) {
			continue;
		}

		auto &group_data = groups[group_idx];
		if (group_data->Count()) {
			hash_group = make_uniq<HashedSortGroup>(client, *hashed_sort.sort, group_idx);
		}
	}

	//	Combine the thread data into the global data
	if (single_key_tracker && local_tracker) {
		single_key_tracker->Combine(*local_tracker);
	}
	grouping_data->Combine(*local_partition);
}

bool HashedSortGlobalSinkState::CanBypassSort(idx_t hash_bin) const {
	if (hashed_sort.integer_partitioning) {
		// every partition but the overflow partition holds a single key
		return grouping_data && grouping_data->Cast<IntegerPartitionedTupleData>().IsSingleKeyPartition(hash_bin);
	}
	return single_key_tracker && single_key_tracker->CanBypass(hash_bin);
}

ProgressData HashedSortGlobalSinkState::GetSinkProgress(ClientContext &client, const ProgressData source) const {
	// the hash groups are sorted after the sink has finished
	return source;
}

SinkFinalizeType HashedSort::Finalize(ClientContext &client, OperatorSinkFinalizeInput &finalize) const {
	auto &gsink = finalize.global_state.Cast<HashedSortGlobalSinkState>();

	//	Did we get any data?
	if (!gsink.count) {
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// OVER(PARTITION BY...)
	auto &partitions = gsink.grouping_data->GetPartitions();
	D_ASSERT(!gsink.hash_groups.empty());
	for (hash_t hash_bin = 0; hash_bin < partitions.size(); ++hash_bin) {
		auto &partition = *partitions[hash_bin];
		if (!partition.Count()) {
			continue;
		}

		auto &hash_group = gsink.hash_groups[hash_bin];
		if (!hash_group) {
			continue;
		}

		// Prepare to scan into the sort
		auto &parallel_scan = hash_group->parallel_scan;
		partition.InitializeScan(parallel_scan, partition_ids);
	}

	return SinkFinalizeType::READY;
}

ProgressData HashedSort::GetSinkProgress(ClientContext &client, GlobalSinkState &gstate,
                                         const ProgressData source) const {
	auto &gsink = gstate.Cast<HashedSortGlobalSinkState>();
	return gsink.GetSinkProgress(client, source);
}

//===--------------------------------------------------------------------===//
// HashedSortLocalSinkState
//===--------------------------------------------------------------------===//
class HashedSortLocalSinkState : public LocalSinkState {
public:
	using LocalSortStatePtr = unique_ptr<LocalSinkState>;
	using GroupingPartition = unique_ptr<PartitionedTupleData>;
	using GroupingAppend = unique_ptr<PartitionedTupleDataAppendState>;

	HashedSortLocalSinkState(ExecutionContext &context, const HashedSort &hashed_sort);

	//! Global state
	const HashedSort &hashed_sort;
	Allocator &allocator;

	//! Shared expression evaluation
	ExpressionExecutor hash_exec;
	ExpressionExecutor sort_exec;
	DataChunk group_chunk;
	DataChunk sort_chunk;
	DataChunk payload_chunk;

	//! Compute the hash values
	void Hash(DataChunk &input_chunk, Vector &hash_vector);
	//! Merge the state into the global state.
	void Combine(ExecutionContext &context);

	//! OVER(PARTITION BY...) (hash grouping)
	GroupingPartition local_grouping;
	GroupingAppend grouping_append;
	unique_ptr<PartitionKeyTracker> single_key_tracker;

	//! (optional) HyperLogLog state
	optional_ptr<ParallelHyperLogLogLocalState> hll;
};

HashedSortLocalSinkState::HashedSortLocalSinkState(ExecutionContext &context, const HashedSort &hashed_sort)
    : hashed_sort(hashed_sort), allocator(Allocator::Get(context.client)), hash_exec(context.client),
      sort_exec(context.client) {
	vector<LogicalType> group_types;
	group_types.reserve(hashed_sort.partitions.size());
	for (idx_t prt_idx = 0; prt_idx < hashed_sort.partitions.size(); prt_idx++) {
		auto &pexpr = *hashed_sort.partitions[prt_idx].expression.get();
		group_types.push_back(hashed_sort.partition_key_types[prt_idx]);
		hash_exec.AddExpression(pexpr);
	}

	vector<LogicalType> sort_types;
	for (const auto &expr : hashed_sort.sort_exprs) {
		sort_types.emplace_back(expr->GetReturnType());
		sort_exec.AddExpression(*expr);
	}
	sort_chunk.Initialize(context.client, sort_types);

	auto hash_types = hashed_sort.payload_types;
	group_chunk.Initialize(allocator, group_types);
	hash_types.emplace_back(LogicalType::HASH);
	payload_chunk.Initialize(allocator, hash_types);

	if (hashed_sort.track_single_keys) {
		single_key_tracker = make_uniq<PartitionKeyTracker>(allocator, group_types);
	}
}

void HashedSort::Synchronize(ClientContext &client, const GlobalSinkState &source, GlobalSinkState &target) const {
	auto &src = source.Cast<HashedSortGlobalSinkState>();
	auto &tgt = target.Cast<HashedSortGlobalSinkState>();
	tgt.SyncPartitioning(src);
}

void HashedSortLocalSinkState::Hash(DataChunk &input_chunk, Vector &hash_vector) {
	D_ASSERT(group_chunk.ColumnCount() > 0);

	// OVER(PARTITION BY...) (hash grouping)
	group_chunk.Reset();
	hash_exec.Execute(input_chunk, group_chunk);
	const idx_t count = group_chunk.size();
	VectorOperations::Hash(group_chunk.data[0], hash_vector, count);
	for (idx_t prt_idx = 1; prt_idx < group_chunk.ColumnCount(); ++prt_idx) {
		VectorOperations::CombineHash(hash_vector, group_chunk.data[prt_idx], count);
	}
}

SinkResultType HashedSort::Sink(ExecutionContext &context, DataChunk &input_chunk, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<HashedSortGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<HashedSortLocalSinkState>();
	gstate.count += input_chunk.size();

	//	Payload prefix is the input data
	auto &payload_chunk = lstate.payload_chunk;
	payload_chunk.Reset();
	for (column_t i = 0; i < input_chunk.ColumnCount(); ++i) {
		payload_chunk.data[i].Reference(input_chunk.data[i]);
	}

	//	Compute any sort columns that are not references and append them to the end of the payload
	auto &sort_chunk = lstate.sort_chunk;
	auto &sort_exec = lstate.sort_exec;
	if (!sort_exprs.empty()) {
		sort_chunk.Reset();
		sort_exec.Execute(input_chunk, sort_chunk);
		for (column_t i = 0; i < sort_chunk.ColumnCount(); ++i) {
			payload_chunk.data[input_chunk.ColumnCount() + i].Reference(sort_chunk.data[i]);
		}
	}

	//	Append a forced payload column
	if (force_payload) {
		auto &vec = payload_chunk.data[input_chunk.ColumnCount() + sort_chunk.ColumnCount()];
		D_ASSERT(vec.GetType().id() == LogicalTypeId::BOOLEAN);
		ConstantVector::SetNull(vec, count_t(input_chunk.size()));
	}

	payload_chunk.SetChildCardinality(input_chunk.size());

	// OVER(PARTITION BY...)
	auto &hash_vector = payload_chunk.data.back();
	lstate.Hash(input_chunk, hash_vector);
	for (idx_t col_idx = 0; col_idx < input_chunk.ColumnCount(); ++col_idx) {
		payload_chunk.data[col_idx].Reference(input_chunk.data[col_idx]);
	}

	// Update HLL state with hashes if necessary
	if (lstate.hll) {
		lstate.hll->Update(hash_vector);
	}

	auto &local_grouping = lstate.local_grouping;
	auto &grouping_append = lstate.grouping_append;
	gstate.UpdateLocalPartition(local_grouping, grouping_append, lstate.single_key_tracker.get());
	local_grouping->Append(*grouping_append, payload_chunk);
	if (lstate.single_key_tracker) {
		lstate.single_key_tracker->Update(lstate.group_chunk, hash_vector, *grouping_append, input_chunk.size());
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType HashedSort::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<HashedSortGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<HashedSortLocalSinkState>();

	// OVER(PARTITION BY...)
	auto &local_grouping = lstate.local_grouping;
	if (!local_grouping) {
		return SinkCombineResultType::FINISHED;
	}

	// Flush our data and lock the bit count
	auto &grouping_append = lstate.grouping_append;
	gstate.CombineLocalPartition(local_grouping, grouping_append, lstate.single_key_tracker.get());

	return SinkCombineResultType::FINISHED;
}

static void BuildDirectColumnData(ClientContext &client, const vector<LogicalType> &payload_types,
                                  TupleDataCollection &partition, HashedSortGroup &hash_group) {
	DataChunk chunk;
	partition.InitializeScanChunk(hash_group.parallel_scan.scan_state, chunk);
	TupleDataLocalScanState local_scan;
	partition.InitializeScan(local_scan);

	// Downstream source tasks address hash groups by chunk index, so all scanning threads append into one
	// collection: Append packs the rows densely, and copying under the lock is cheaper than a second copy
	idx_t combined = 0;
	while (partition.Scan(hash_group.parallel_scan, local_scan, chunk)) {
		lock_guard<mutex> direct_guard(hash_group.scan_lock);
		if (!hash_group.direct_column_data) {
			hash_group.direct_column_data = make_uniq<ColumnDataCollection>(
			    client, payload_types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
			hash_group.direct_column_data->InitializeAppend(hash_group.direct_append_state);
		}
		hash_group.direct_column_data->Append(hash_group.direct_append_state, chunk);
		combined += chunk.size();
	}
	const auto hash_group_count = hash_group.count += combined;
	if (hash_group_count == partition.Count()) {
		// the last scanning task releases the blocks the append state keeps pinned
		lock_guard<mutex> direct_guard(hash_group.scan_lock);
		hash_group.direct_append_state = ColumnDataAppendState();
	}
}

void HashedSort::SortColumnData(ExecutionContext &context, hash_t hash_bin, OperatorSinkFinalizeInput &finalize) const {
	auto &gstate = finalize.global_state.Cast<HashedSortGlobalSinkState>();

	//	Loop over the partitions and add them to each hash group's global sort state
	auto &partitions = gstate.grouping_data->GetPartitions();
	if (hash_bin >= partitions.size()) {
		return;
	}

	auto &partition = *partitions[hash_bin];
	if (!partition.Count()) {
		return;
	}

	auto &hash_group = *gstate.hash_groups[hash_bin];

	if (gstate.CanBypassSort(hash_bin)) {
		BuildDirectColumnData(context.client, payload_types, partition, hash_group);
		return;
	}

	auto &parallel_scan = hash_group.parallel_scan;
	DataChunk chunk;
	partition.InitializeScanChunk(parallel_scan.scan_state, chunk);
	TupleDataLocalScanState local_scan;
	partition.InitializeScan(local_scan);

	auto sort_local = sort->GetLocalSinkState(context);
	OperatorSinkInput sink {*hash_group.sort_global, *sort_local, finalize.interrupt_state};
	idx_t combined = 0;
	while (partition.Scan(hash_group.parallel_scan, local_scan, chunk)) {
		sort->Sink(context, chunk, sink);
		combined += chunk.size();
	}

	OperatorSinkCombineInput combine {*hash_group.sort_global, *sort_local, finalize.interrupt_state};
	sort->Combine(context, combine);
	hash_group.count += combined;

	//	Whoever finishes last can Finalize
	lock_guard<mutex> finalize_guard(hash_group.scan_lock);
	if (hash_group.count == partition.Count() && !hash_group.sort_source) {
		OperatorSinkFinalizeInput lfinalize {*hash_group.sort_global, finalize.interrupt_state};
		sort->Finalize(context.client, lfinalize);
		hash_group.sort_source = sort->GetGlobalSourceState(context.client, *hash_group.sort_global);
	}
}

//===--------------------------------------------------------------------===//
// HashedSortGlobalSourceState
//===--------------------------------------------------------------------===//
class HashedSortGlobalSourceState : public GlobalSourceState {
public:
	using HashGroupPtr = unique_ptr<ColumnDataCollection>;
	using SortedRunPtr = unique_ptr<SortedRun>;
	using ChunkRow = HashedSort::ChunkRow;
	using ChunkRows = HashedSort::ChunkRows;

	HashedSortGlobalSourceState(ClientContext &client, HashedSortGlobalSinkState &gsink);

	HashedSortGlobalSinkState &gsink;
	ChunkRows chunk_rows;
};

HashedSortGlobalSourceState::HashedSortGlobalSourceState(ClientContext &client, HashedSortGlobalSinkState &gsink)
    : gsink(gsink) {
	if (!gsink.count) {
		return;
	}

	auto &partitions = gsink.grouping_data->GetPartitions();
	for (hash_t hash_bin = 0; hash_bin < partitions.size(); ++hash_bin) {
		ChunkRow chunk_row;

		auto &hash_group = gsink.hash_groups[hash_bin];
		if (hash_group) {
			chunk_row.count = partitions[hash_bin]->Count();
			chunk_row.chunks = (chunk_row.count + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
		}

		chunk_rows.emplace_back(chunk_row);
	}
}

//===--------------------------------------------------------------------===//
// HashedSort
//===--------------------------------------------------------------------===//
void HashedSort::GenerateOrderings(Orders &partitions, Orders &orders,
                                   const vector<unique_ptr<Expression>> &partition_bys, const Orders &order_bys,
                                   const vector<unique_ptr<BaseStatistics>> &partition_stats) {
	// we sort by both 1) partition by expression list and 2) order by expressions
	const auto partition_cols = partition_bys.size();
	for (idx_t prt_idx = 0; prt_idx < partition_cols; prt_idx++) {
		auto &pexpr = partition_bys[prt_idx];

		if (partition_stats.empty() || !partition_stats[prt_idx]) {
			orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, pexpr->Copy(), nullptr);
		} else {
			orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, pexpr->Copy(),
			                    partition_stats[prt_idx]->ToUnique());
		}
		partitions.emplace_back(orders.back().Copy());
	}

	for (const auto &order : order_bys) {
		orders.emplace_back(order.Copy());
	}
}

HashedSort::HashedSort(ClientContext &client, const vector<unique_ptr<Expression>> &partition_bys,
                       const vector<BoundOrderByNode> &order_bys, const Types &input_types,
                       const vector<unique_ptr<BaseStatistics>> &partition_stats, idx_t estimated_cardinality,
                       bool require_payload)
    : SortStrategy(input_types), estimated_cardinality(estimated_cardinality) {
	GenerateOrderings(partitions, orders, partition_bys, order_bys, partition_stats);
	partition_key_count = partitions.size();
	can_bypass_single_key_sort = partition_key_count > 0 && order_bys.empty();
	partition_key_types.reserve(partition_key_count);
	for (idx_t key_idx = 0; key_idx < partition_key_count; key_idx++) {
		partition_key_types.push_back(partitions[key_idx].expression->GetReturnType());
	}

	//	We have to compute ordering expressions ourselves and materialise them.
	//	To do this, we scan the orders and add generate extra payload columns that we can reference.
	for (auto &order : orders) {
		auto &expr = *order.expression;
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			auto &ref = expr.Cast<BoundReferenceExpression>();
			sort_ids.emplace_back(ref.Index());
			continue;
		}

		//	Real expression - replace with a ref and save the expression
		auto saved = std::move(order.expression);
		const auto type = saved->GetReturnType();
		const auto idx = payload_types.size();
		order.expression = make_uniq<BoundReferenceExpression>(type, idx);
		sort_ids.emplace_back(idx);
		payload_types.emplace_back(type);
		sort_exprs.emplace_back(std::move(saved));
	}
	for (idx_t key_idx = 0; key_idx < partition_key_count; key_idx++) {
		partition_key_ids.push_back(sort_ids[key_idx]);
	}

	//	A single integer partition key with known bounds is partitioned on directly: every group is single-key
	if (can_bypass_single_key_sort && partition_key_count == 1) {
		integer_partitioning = IntegerPartitionedTupleData::TryBoundedKey(
		    client, partition_key_types[0], partitions[0].stats.get(), integer_partition_min, integer_partition_count);
	}
	track_single_keys = can_bypass_single_key_sort && !integer_partitioning;

	// If a payload column is required, check whether there is one already
	if (require_payload) {
		//	Watch out for duplicate sort keys!
		unordered_set<column_t> sort_set(sort_ids.begin(), sort_ids.end());
		force_payload = (sort_set.size() >= payload_types.size());
		if (force_payload) {
			payload_types.emplace_back(LogicalType::BOOLEAN);
		}
	}

	// Remember the full set of materialised partition columns
	for (column_t i = 0; i < payload_types.size(); ++i) {
		partition_ids.emplace_back(i);
	}

	vector<idx_t> projection_map;
	sort = make_uniq<Sort>(client, orders, payload_types, projection_map);
}

unique_ptr<GlobalSinkState> HashedSort::GetGlobalSinkState(ClientContext &client) const {
	return make_uniq<HashedSortGlobalSinkState>(client, *this);
}

unique_ptr<LocalSinkState> HashedSort::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<HashedSortLocalSinkState>(context, *this);
}

void HashedSort::RegisterHyperLogLog(LocalSinkState &local_state, ParallelHyperLogLogLocalState &hll_state) const {
	auto &lstate = local_state.Cast<HashedSortLocalSinkState>();
	lstate.hll = hll_state;
}

unique_ptr<GlobalSourceState> HashedSort::GetGlobalSourceState(ClientContext &client, GlobalSinkState &sink) const {
	return make_uniq<HashedSortGlobalSourceState>(client, sink.Cast<HashedSortGlobalSinkState>());
}

const HashedSort::ChunkRows &HashedSort::GetHashGroups(GlobalSourceState &gstate) const {
	auto &gsource = gstate.Cast<HashedSortGlobalSourceState>();
	return gsource.chunk_rows;
}

unique_ptr<TupleDataCollection> HashedSort::TryTakeSinglePartition(idx_t hash_bin, GlobalSourceState &source) const {
	auto &gsource = source.Cast<HashedSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	if (!gsink.grouping_data || !gsink.CanBypassSort(hash_bin)) {
		return nullptr;
	}
	lock_guard<mutex> guard(gsink.lock);
	auto &partitions = gsink.grouping_data->GetPartitions();
	if (hash_bin >= partitions.size() || !partitions[hash_bin] || !partitions[hash_bin]->Count()) {
		return nullptr;
	}
	auto result = std::move(partitions[hash_bin]);
	// the bin leaves the sort path: its sort state is not needed
	gsink.hash_groups[hash_bin].reset();
	return result;
}

static SourceResultType MaterializeHashGroupData(ExecutionContext &context, idx_t hash_bin, bool build_runs,
                                                 OperatorSourceInput &source) {
	auto &gsource = source.global_state.Cast<HashedSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_groups[hash_bin];

	D_ASSERT(!build_runs || !gsink.CanBypassSort(hash_bin));

	//	OVER(PARTITION BY...)
	if (gsink.grouping_data) {
		lock_guard<mutex> reset_guard(hash_group.scan_lock);
		auto &partitions = gsink.grouping_data->GetPartitions();
		if (hash_bin < partitions.size()) {
			//	Release the memory now that we have finished scanning it.
			partitions[hash_bin].reset();
		}
	}

	if (!build_runs && gsink.CanBypassSort(hash_bin)) {
		return SourceResultType::FINISHED;
	}

	auto &sort = hash_group.sort;
	auto &sort_global = *hash_group.sort_source;
	auto sort_local = sort.GetLocalSourceState(context, sort_global);

	OperatorSourceInput input {sort_global, *sort_local, source.interrupt_state};
	if (build_runs) {
		return sort.MaterializeSortedRun(context, input);
	} else {
		return sort.MaterializeColumnData(context, input);
	}
}

SourceResultType HashedSort::MaterializeColumnData(ExecutionContext &execution, idx_t hash_bin,
                                                   OperatorSourceInput &source) const {
	return MaterializeHashGroupData(execution, hash_bin, false, source);
}

HashedSort::HashGroupPtr HashedSort::GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<HashedSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_groups[hash_bin];

	if (gsink.CanBypassSort(hash_bin)) {
		auto result = std::move(hash_group.direct_column_data);
		if (result && result->Count() == hash_group.count) {
			return result;
		}
		return nullptr;
	}

	auto &sort = hash_group.sort;
	auto &sort_global = *hash_group.sort_source;

	OperatorSourceInput input {sort_global, source.local_state, source.interrupt_state};
	auto result = sort.GetColumnData(input);
	hash_group.sort_source.reset();

	//	Just because MaterializeColumnData returned FINISHED doesn't mean that the same thread will
	//	get the result...
	if (result && result->Count() == hash_group.count) {
		return result;
	}

	return nullptr;
}

SourceResultType HashedSort::MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
                                                  OperatorSourceInput &source) const {
	return MaterializeHashGroupData(context, hash_bin, true, source);
}

HashedSort::SortedRunPtr HashedSort::GetSortedRun(ClientContext &client, idx_t hash_bin,
                                                  OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<HashedSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_groups[hash_bin];

	D_ASSERT(!gsink.CanBypassSort(hash_bin));

	auto &sort = hash_group.sort;
	auto &sort_global = *hash_group.sort_source;

	auto result = sort.GetSortedRun(sort_global);
	if (!result) {
		D_ASSERT(hash_group.count == 0);
		result = make_uniq<SortedRun>(client, sort, false);
	}

	hash_group.sort_source.reset();

	return result;
}

} // namespace duckdb
