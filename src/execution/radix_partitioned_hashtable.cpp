#include "duckdb/execution/radix_partitioned_hashtable.hpp"
#include "duckdb/execution/radix_ht_sink_state.hpp"
#include "duckdb/execution/radix_ht_adaptivity.hpp"

#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/enums/debug_verification_mode.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/aggregate_state_spilling.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

RadixPartitionedHashTable::RadixPartitionedHashTable(GroupingSet &grouping_set_p, const GroupedAggregateData &op_p,
                                                     TupleDataValidityType group_validity_p)
    : grouping_set(grouping_set_p), op(op_p), group_validity(group_validity_p) {
	auto groups_count = op.GroupCount();
	for (auto group_idx : ProjectionIndex::GetIndexes(groups_count)) {
		if (grouping_set.find(group_idx) == grouping_set.end()) {
			null_groups.push_back(group_idx);
		}
	}
	if (grouping_set.empty()) {
		// Fake a single group with a constant value for aggregation without groups
		group_types.emplace_back(LogicalType::TINYINT);
	}
	for (auto &entry : grouping_set) {
		group_types.push_back(op.group_types[entry]);
	}
	SetGroupingValues();

	auto group_types_copy = group_types;
	group_types_copy.emplace_back(LogicalType::HASH);

	auto layout = make_shared_ptr<TupleDataLayout>();
	auto aggregate_objects = AggregateObject::CreateAggregateObjects(op.bindings);
	layout->Initialize(std::move(group_types_copy), std::move(aggregate_objects), group_validity);
	layout_ptr = std::move(layout);
}

void RadixPartitionedHashTable::SetGroupingValues() {
	// Compute the GROUPING values:
	// For each parameter to the GROUPING clause, we check if the hash table groups on this particular group
	// If it does, we return 0, otherwise we return 1
	// We then use bitshifts to combine these values
	auto &grouping_functions = op.GetGroupingFunctions();
	for (auto &grouping : grouping_functions) {
		int64_t grouping_value = 0;
		D_ASSERT(grouping.size() < sizeof(int64_t) * 8);
		for (idx_t i = 0; i < grouping.size(); i++) {
			if (grouping_set.find(grouping[i]) == grouping_set.end()) {
				// We don't group on this value!
				grouping_value += 1LL << (grouping.size() - (i + 1));
			}
		}
		grouping_values.push_back(Value::BIGINT(grouping_value));
	}
}

shared_ptr<TupleDataLayout> RadixPartitionedHashTable::GetLayoutPtr() const {
	return layout_ptr;
}

const TupleDataLayout &RadixPartitionedHashTable::GetLayout() const {
	return *layout_ptr;
}

unique_ptr<GroupedAggregateHashTable> RadixPartitionedHashTable::CreateHT(ClientContext &context, const idx_t capacity,
                                                                          const idx_t radix_bits) const {
	return make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), group_types, op.payload_types,
	                                            op.bindings, capacity, radix_bits, group_validity);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
enum class AggregatePartitionState : uint8_t {
	//! Can be finalized
	READY_TO_FINALIZE = 0,
	//! Finalize is in progress
	FINALIZE_IN_PROGRESS = 1,
	//! Finalized, ready to scan
	READY_TO_SCAN = 2
};

struct AggregatePartition : StateWithBlockableTasks {
	explicit AggregatePartition(unique_ptr<TupleDataCollection> data_p)
	    : state(AggregatePartitionState::READY_TO_FINALIZE), data(std::move(data_p)) {
	}

	AggregatePartitionState state;

	unique_ptr<TupleDataCollection> data;
	//! Data of this partition whose aggregate states were exported to spillable storage
	unique_ptr<ColumnDataCollection> exported_data;
	//! When states spill, each partition holds its own arenas, released once the partition is scanned
	shared_ptr<ArenaAllocator> allocator;
	//! Arena holding the imported states (combining may steal from them, so it must live as long)
	shared_ptr<ArenaAllocator> import_allocator;
	//! Combine progress of this partition
	atomic<idx_t> combine_chunk_count {0};
	atomic<idx_t> combined_chunks {0};
	atomic<bool> finalized {false};

	double GetCombineProgress() const {
		if (finalized.load(std::memory_order_relaxed)) {
			return 1;
		}
		auto chunk_count = combine_chunk_count.load(std::memory_order_relaxed);
		if (chunk_count == 0) {
			return 0;
		}
		auto combined = MinValue<idx_t>(combined_chunks.load(std::memory_order_relaxed), chunk_count);
		return static_cast<double>(combined) / static_cast<double>(chunk_count);
	}
};

RadixHTGlobalSinkState::RadixHTGlobalSinkState(ClientContext &context_p, const RadixPartitionedHashTable &radix_ht_p)
    : context(context_p), temporary_memory_state(TemporaryMemoryManager::Get(context).Register(context)),
      finalized(false), external(false), iteration_reused(false), active_threads(0),
      number_of_threads(TaskScheduler::GetScheduler(context).NumberOfThreads()),
      memory_limit(BufferManager::GetBufferManager(context).GetOperatorMemoryLimit()),
      block_alloc_size(BufferManager::GetBufferManager(context).GetBlockAllocSize()), any_combined(false),
      any_abandoned(false), radix_ht(radix_ht_p),
      spill_plan(AggregateStateSpilling::TryCreateSpillPlan(radix_ht.GetLayout())), config(*this),
      stored_allocators_size(0), finalize_done(0), scan_pin_properties(TupleDataPinProperties::DESTROY_AFTER_DONE),
      count_before_combining(0), max_partition_size(0) {
	spill_phase = SpillPhase::NATIVE_ALLOWED;

	SetReservationFloor(context, MaxValue(config.GetRadixBits(), config.GetMaximumSinkRadixBits()));
}

void RadixHTGlobalSinkState::SetReservationFloor(ClientContext &context_p, const idx_t radix_bits) {
	minimum_reservation = CalculateMinimumReservation(radix_bits);
	temporary_memory_state->SetMinimumReservation(minimum_reservation);
	temporary_memory_state->SetRemainingSizeAndUpdateReservation(context_p, minimum_reservation);
}

void RadixHTGlobalSinkState::ResetConfigForNewIteration(ClientContext &context_p, const idx_t scheduled_workers) {
	iteration_reused = true;
	config.ResetForNewIteration(scheduled_workers);
	// The restart partition count determines the reservation floor. This also undoes the scaling that
	// SetRadixBitsInternal applied if a previous iteration went external.
	SetReservationFloor(context_p, config.GetRadixBits());
}

idx_t RadixHTGlobalSinkState::CalculateMinimumReservation(const idx_t radix_bits) const {
	const auto tuples_per_block = block_alloc_size / radix_ht.GetLayout().GetRowWidth();
	const auto ht_count =
	    LossyNumericCast<idx_t>(static_cast<double>(config.sink_capacity) / GroupedAggregateHashTable::LOAD_FACTOR);
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	const auto count_per_partition = ht_count / num_partitions;
	auto blocks_per_partition = (count_per_partition + tuples_per_block) / tuples_per_block;
	if (!radix_ht.GetLayout().AllConstant()) {
		blocks_per_partition += 1;
	}
	const auto ht_size =
	    num_partitions * blocks_per_partition * block_alloc_size + config.sink_capacity * sizeof(ht_entry_t);

	// This really is the minimum reservation that we can do
	return number_of_threads * ht_size;
}

RadixHTGlobalSinkState::~RadixHTGlobalSinkState() {
	Destroy();
}

// LCOV_EXCL_START
void RadixHTGlobalSinkState::Destroy() {
	if (scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE || count_before_combining == 0 ||
	    partitions.empty()) {
		// Already destroyed / empty
		return;
	}

	TupleDataLayout layout = partitions[0]->data->GetLayout().Copy();
	if (!layout.HasDestructor()) {
		return; // No destructors, exit
	}

	// There are aggregates with destructors: Call the destructor for each of the aggregates
	const annotated_lock_guard<annotated_mutex> guard {lock};
	RowOperationsState row_state(*stored_allocators.back());
	for (auto &partition : partitions) {
		auto &data_collection = *partition->data;
		if (data_collection.Count() == 0) {
			continue;
		}
		TupleDataChunkIterator iterator(data_collection, TupleDataPinProperties::DESTROY_AFTER_DONE, false);
		auto &row_locations = iterator.GetChunkState().row_locations;
		do {
			RowOperations::DestroyStates(row_state, layout, row_locations);
		} while (iterator.Next());
		data_collection.Reset();
	}
}
// LCOV_EXCL_STOP

RadixHTConfig::RadixHTConfig(RadixHTGlobalSinkState &sink_p)
    : sink(sink_p), row_width(sink.radix_ht.GetLayout().GetRowWidth()), sink_capacity(SinkCapacity()),
      sink_radix_bits(InitialSinkRadixBits(sink.number_of_threads)), maximum_sink_radix_bits(MaximumSinkRadixBits()) {
}

void RadixHTConfig::ResetForNewIteration(const idx_t scheduled_workers) {
	// Recursive iterations mostly run few workers over small frontiers: size the restart from the
	// workers the scheduler actually assigns instead of the thread-count-sized default, down to a
	// single partition for serial iterations. SetRadixBits grows the configuration again when an
	// iteration carries enough data.
	sink_radix_bits = InitialSinkRadixBits(MaxValue<idx_t>(scheduled_workers, 1));
}

void RadixHTConfig::SetRadixBits(const idx_t &radix_bits_p) {
	const auto max_bits = MinValue(maximum_sink_radix_bits, ExternalRadixBits(true));
	SetRadixBitsInternal(MinValue(radix_bits_p, max_bits), false);
}

bool RadixHTConfig::SetRadixBitsToExternal() {
	SetRadixBitsInternal(ExternalRadixBits(true), true);
	return sink.external;
}

idx_t RadixHTConfig::GetRadixBits() const {
	return sink_radix_bits;
}

idx_t RadixHTConfig::GetMaximumSinkRadixBits() const {
	return maximum_sink_radix_bits;
}

void RadixHTConfig::SetRadixBitsInternal(const idx_t radix_bits_p, bool external) {
	if ((!external && sink_radix_bits > radix_bits_p) || sink.any_combined) {
		return;
	}

	const annotated_lock_guard<annotated_mutex> guard {sink.lock};
	if ((!external && sink_radix_bits > radix_bits_p) || sink.any_combined) {
		return;
	}

	const auto radix_bits = MaxValue(radix_bits_p, sink_radix_bits.load());
	if (external) {
		const auto partition_multiplier =
		    RadixPartitioning::NumberOfPartitions(radix_bits) / RadixPartitioning::NumberOfPartitions(sink_radix_bits);
		sink.minimum_reservation = sink.minimum_reservation * partition_multiplier;
		sink.external = true;
	} else if (sink.iteration_reused && radix_bits > sink_radix_bits) {
		// A recursive iteration can restart below the maximum partition count: growing the partitions
		// grows the working set, so the reservation floor must cover the new configuration again.
		sink.minimum_reservation =
		    MaxValue<idx_t>(sink.minimum_reservation, sink.CalculateMinimumReservation(radix_bits));
	}
	if (sink.iteration_reused) {
		// Publish the raised floor. UpdateSinkReservation can have published a larger minimum under this
		// lock, so never lower the published value here. Ordinary (non-reused) sinks keep their floor
		// private, as before.
		auto &temporary_memory_state = *sink.temporary_memory_state;
		temporary_memory_state.SetMinimumReservation(
		    MaxValue<idx_t>(temporary_memory_state.GetMinimumReservation(), sink.minimum_reservation));
	}

	sink_radix_bits = radix_bits;
}

idx_t RadixHTConfig::InitialSinkRadixBits(const idx_t num_threads) const {
	return MinValue(RadixPartitioning::RadixBitsOfPowerOfTwo(NextPowerOfTwo(num_threads)),
	                MAXIMUM_INITIAL_SINK_RADIX_BITS);
}

idx_t RadixHTConfig::ExternalRadixBits(const bool dynamic) const {
	// Going to many partitions is great for reducing memory usage during the GetData phase
	// However, we can't go to, e.g., 256 partitions when we have 8 threads and 200 MiB of memory
	// Because we'll have too many pages in memory to do the partitioning in the first place

	// Budget row blocks and optional heap or exported-state blocks, with headroom for repartitioning.
	const auto memory_limit = dynamic ? sink.temporary_memory_state->GetReservation() : sink.memory_limit / 2;
	const idx_t pages_per_partition = sink.radix_ht.GetLayout().AllConstant() && !sink.spill_plan ? 2 : 4;
	const auto max_partitions = memory_limit / sink.number_of_threads / sink.block_alloc_size / pages_per_partition;

	// Compute number of bits, rounded down, at least as much as initial bits
	const auto bits =
	    MaxValue(RadixPartitioning::RadixBits(max_partitions) - 1, InitialSinkRadixBits(sink.number_of_threads));

	// Avoid returning 0 or underflowed bits
	if (max_partitions == 0 || bits == 0) {
		return 1;
	}

	// Capped by global maximum
	return MinValue(bits, MAXIMUM_FINAL_SINK_RADIX_BITS);
}

idx_t RadixHTConfig::MaximumSinkRadixBits() const {
	if (sink.number_of_threads <= GROW_STRATEGY_THREAD_THRESHOLD) {
		return InitialSinkRadixBits(sink.number_of_threads); // Don't repartition unless we go external
	}
	// If rows are very wide we have to reduce the number of partitions, otherwise cache misses get out of hand
	idx_t bits = DConstants::INVALID_INDEX;
	if (row_width >= ROW_WIDTH_THRESHOLD_TWO) {
		bits = MAXIMUM_FINAL_SINK_RADIX_BITS - 2;
	} else if (row_width >= ROW_WIDTH_THRESHOLD_ONE) {
		bits = MAXIMUM_FINAL_SINK_RADIX_BITS - 1;
	} else {
		bits = MAXIMUM_FINAL_SINK_RADIX_BITS;
	}
	// Capped by external radix bits
	return MinValue(bits, ExternalRadixBits(false));
}

idx_t RadixHTConfig::SinkCapacity() const {
	if (sink.number_of_threads <= GROW_STRATEGY_THREAD_THRESHOLD) {
		// Grow strategy, start off a bit bigger
		return 262144;
	}
	// Start with a small local table and grow when repeated groups justify it
	return 32768;
}

RadixHTLocalSinkState::RadixHTLocalSinkState(ClientContext &, const RadixPartitionedHashTable &radix_ht)
    : registered(false), local_sink_capacity(DConstants::INVALID_INDEX) {
	// If there are no groups we create a fake group so everything has the same group
	group_chunk.InitializeEmpty(radix_ht.group_types);
	if (radix_ht.grouping_set.empty()) {
		group_chunk.data[0].Reference(Value::TINYINT(42), count_t(STANDARD_VECTOR_SIZE));
	}
}

void RadixHTLocalSinkState::PrepareForSpill(RadixHTGlobalSinkState &gstate) {
	ht->EnableHLL(false);
	gstate.any_abandoned = true;
	ht->Abandon();
	if (!spilling) {
		// Iteration reuse can retain a larger allocation than the logical capacity.
		try {
			ht->Resize(gstate.config.sink_capacity);
		} catch (const OutOfMemoryException &) {
			// Keep the existing table if the temporary replacement allocation cannot fit.
		}
		spilling = true;
	}
	local_sink_capacity = ht->Capacity();
}

void RadixHTLocalSinkState::ResetForReuse(const RadixPartitionedHashTable &radix_ht, RadixHTGlobalSinkState &gstate) {
	group_chunk.Reset();
	if (radix_ht.grouping_set.empty()) {
		group_chunk.data[0].Reference(Value::TINYINT(42), count_t(STANDARD_VECTOR_SIZE));
	}
	registered = false;
	spilling = false;
	abandoned_data.reset();
	abandoned_exported_data.clear();
	if (!ht) {
		local_sink_capacity = DConstants::INVALID_INDEX;
		return;
	}

	ht->ResetForNewIteration(gstate.config.GetRadixBits());
	local_sink_capacity = MaxValue(gstate.config.sink_capacity, ht->Capacity());
	if (gstate.number_of_threads > RadixHTConfig::GROW_STRATEGY_THREAD_THRESHOLD) {
		ht->EnableHLL(true);
	}
}

unique_ptr<GlobalSinkState> RadixPartitionedHashTable::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<RadixHTGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> RadixPartitionedHashTable::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<RadixHTLocalSinkState>(context.client, *this);
}

void RadixPartitionedHashTable::ResetGlobalSinkState(ClientContext &context, GlobalSinkState &gstate_p,
                                                     const idx_t scheduled_workers) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();
	gstate.Destroy();
	gstate.ResetConfigForNewIteration(context, scheduled_workers);
	gstate.finalized = false;
	gstate.external = false;
	gstate.active_threads = 0;
	gstate.any_combined = false;
	gstate.any_abandoned = false;
	gstate.uncombined_data.reset();
	gstate.uncombined_exported_data.clear();
	{
		const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
		gstate.spill_phase = SpillPhase::NATIVE_ALLOWED;
	}
	gstate.stored_allocators.clear();
	gstate.stored_allocators_size = 0;
	gstate.partitions.clear();
	gstate.finalize_done = 0;
	gstate.scan_pin_properties = TupleDataPinProperties::DESTROY_AFTER_DONE;
	gstate.count_before_combining = 0;
	gstate.max_partition_size = 0;
}

void RadixPartitionedHashTable::AdjustScheduledWorkers(ClientContext &context, GlobalSinkState &gstate_p,
                                                       const idx_t scheduled_workers) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();
	const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
	if (gstate.active_threads != 0 || gstate.any_combined || gstate.uncombined_data || gstate.external) {
		// data already flowed into this sink, so the configuration can no longer be replaced
		return;
	}
	gstate.ResetConfigForNewIteration(context, scheduled_workers);
}

void RadixPartitionedHashTable::ResetLocalSinkState(ExecutionContext &context, GlobalSinkState &gstate_p,
                                                    LocalSinkState &lstate_p) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();
	auto &lstate = lstate_p.Cast<RadixHTLocalSinkState>();
	lstate.ResetForReuse(*this, gstate);
}

void RadixPartitionedHashTable::PopulateGroupChunk(DataChunk &group_chunk, DataChunk &input_chunk) const {
	idx_t chunk_index = 0;
	// Populate the group_chunk
	for (auto &group_idx : grouping_set) {
		// Retrieve the expression containing the index in the input chunk
		auto &group = op.groups[group_idx];
		D_ASSERT(group->GetExpressionType() == ExpressionType::BOUND_REF);
		auto &bound_ref_expr = group->Cast<BoundReferenceExpression>();
		// Reference from input_chunk[group.index] -> group_chunk[chunk_index]
		group_chunk.data[chunk_index++].Reference(input_chunk.data[bound_ref_expr.Index()]);
	}
	group_chunk.SetChildCardinality(input_chunk.size());
	// the fake group for empty grouping_set was created with v_size=STANDARD_VECTOR_SIZE - resize to match
	if (grouping_set.empty()) {
		FlatVector::SetSize(group_chunk.data[0], count_t(input_chunk.size()));
	}
	group_chunk.Verify();
}

// Grow abandoned_data (frozen at the radix bits of when we first went external) up to the current radix bits, so it
// never has fewer partitions than the data merged into it - Repartition/Combine only handle new >= old.
void GrowAbandonedDataToRadixBits(ClientContext &context, RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate,
                                  const idx_t radix_bits) {
	if (!lstate.abandoned_data ||
	    lstate.abandoned_data->PartitionCount() >= RadixPartitioning::NumberOfPartitions(radix_bits)) {
		return;
	}
	auto new_abandoned_data = make_uniq<RadixPartitionedTupleData>(
	    BufferManager::GetBufferManager(context), gstate.radix_ht.GetLayoutPtr(), MemoryTag::HASH_TABLE, radix_bits,
	    gstate.radix_ht.GetLayout().ColumnCount() - 1, context);
	lstate.abandoned_data->Repartition(context, *new_abandoned_data);
	lstate.abandoned_data = std::move(new_abandoned_data);
}

// Grow a set of exported collections to a larger partition count by splitting each collection
// sequentially, so that only one source and its split targets are ever appended to at a time
void GrowExportedData(ClientContext &context, RadixHTGlobalSinkState &gstate,
                      vector<unique_ptr<ColumnDataCollection>> &exported, idx_t new_partition_count) {
	const auto old_partition_count = exported.size();
	D_ASSERT(new_partition_count > old_partition_count);
	D_ASSERT(new_partition_count % old_partition_count == 0);
	const auto fan_out = new_partition_count / old_partition_count;
	const auto radix_bits = RadixPartitioning::RadixBitsOfPowerOfTwo(new_partition_count);
	const auto hash_col_idx = gstate.radix_ht.GetLayout().ColumnCount() - 1;

	vector<unique_ptr<ColumnDataCollection>> grown(new_partition_count);
	DataChunk chunk;
	DataChunk target_chunk;
	for (idx_t old_idx = 0; old_idx < old_partition_count; old_idx++) {
		if (!exported[old_idx] || exported[old_idx]->Count() == 0) {
			continue;
		}
		auto &source = *exported[old_idx];
		if (chunk.data.empty()) {
			chunk.Initialize(Allocator::Get(context), source.Types());
			target_chunk.Initialize(Allocator::Get(context), source.Types());
		}
		ColumnDataScanState scan_state;
		source.InitializeScan(scan_state, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
		SelectionVector sel(STANDARD_VECTOR_SIZE);
		while (source.Scan(scan_state, chunk)) {
			const auto count = chunk.size();
			const auto hashes = FlatVector::GetData<hash_t>(chunk.data[hash_col_idx]);
			for (idx_t target = 0; target < fan_out; target++) {
				const idx_t target_idx = old_idx * fan_out + target;
				idx_t sel_count = 0;
				for (idx_t i = 0; i < count; i++) {
					if (RadixPartitioning::ApplyMask(hashes[i], radix_bits) == target_idx) {
						sel.set_index(sel_count++, i);
					}
				}
				if (sel_count == 0) {
					continue;
				}
				if (!grown[target_idx]) {
					grown[target_idx] =
					    make_uniq<ColumnDataCollection>(BufferManager::GetBufferManager(context), source.Types());
				}
				target_chunk.Reset();
				target_chunk.Slice(chunk, sel, sel_count);
				grown[target_idx]->Append(target_chunk);
			}
		}
		exported[old_idx].reset();
	}
	exported = std::move(grown);
}

// Export the abandoned states to spillable storage, so that their arena can be freed.
// Every abandoned partition is exported sequentially into its own column collection, so
// only one collection is being appended to at any time.
void ExportAbandonedData(ClientContext &context, RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate) {
	if (!lstate.abandoned_data || lstate.abandoned_data->Count() == 0) {
		return;
	}
	const auto partition_count = lstate.abandoned_data->PartitionCount();
	const auto radix_bits = RadixPartitioning::RadixBitsOfPowerOfTwo(partition_count);
	const auto hash_col_idx = gstate.radix_ht.GetLayout().ColumnCount() - 1;
	if (!lstate.abandoned_exported_data.empty() && lstate.abandoned_exported_data.size() < partition_count) {
		// The abandoned data has grown to more radix bits since we last exported, grow to match
		GrowExportedData(context, gstate, lstate.abandoned_exported_data, partition_count);
	}
	if (lstate.abandoned_exported_data.size() < partition_count) {
		lstate.abandoned_exported_data.resize(partition_count);
	}
	auto exported_radix_bits = RadixPartitioning::RadixBitsOfPowerOfTwo(lstate.abandoned_exported_data.size());
	ArenaAllocator scratch_allocator(Allocator::Get(context));
	auto &partitions = lstate.abandoned_data->GetPartitions();
	for (idx_t partition_idx = 0; partition_idx < partition_count; partition_idx++) {
		auto &partition = *partitions[partition_idx];
		if (partition.Count() == 0) {
			continue;
		}
		AggregateStateSpilling::ExportStates(context, gstate.radix_ht.GetLayout(), *gstate.spill_plan, partition,
		                                     lstate.abandoned_exported_data, exported_radix_bits, scratch_allocator);
	}
	// The finalize phase imports and combines one exported partition at a time, so their size
	// must stay well below the memory limit: grow the exported partitions when they get too big
	idx_t exported_bytes = 0;
	for (auto &exported : lstate.abandoned_exported_data) {
		exported_bytes += exported ? exported->SizeInBytes() : 0;
	}
	while (exported_bytes / lstate.abandoned_exported_data.size() >
	           gstate.GetThreadLimit() / RadixHTConfig::AGGREGATE_STATE_SPILL_DIVISOR &&
	       exported_radix_bits + RadixHTConfig::REPARTITION_RADIX_BITS <=
	           RadixHTConfig::MAXIMUM_FINAL_SINK_RADIX_BITS) {
		{
			// Widening beyond the native width forces every subsequent combine to export its
			// rows. Once rows were combined natively, the exported width must stay instead.
			const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
			if (gstate.spill_phase == SpillPhase::NATIVE_COMBINE_STARTED) {
				break;
			}
			gstate.spill_phase = SpillPhase::EXPORTED_ONLY;
		}
		exported_radix_bits += RadixHTConfig::REPARTITION_RADIX_BITS;
		GrowExportedData(context, gstate, lstate.abandoned_exported_data,
		                 RadixPartitioning::NumberOfPartitions(exported_radix_bits));
	}
	// The source rows were destroyed as they were exported, start over with an empty collection
	lstate.abandoned_data =
	    make_uniq<RadixPartitionedTupleData>(BufferManager::GetBufferManager(context), gstate.radix_ht.GetLayoutPtr(),
	                                         MemoryTag::HASH_TABLE, radix_bits, hash_col_idx, context);
}

bool RadixHTGlobalSinkState::StatePressureExceeded(GroupedAggregateHashTable &ht) const {
	return ht.GetAggregateAllocator()->AllocationSize() >
	       GetThreadLimit() / RadixHTConfig::AGGREGATE_STATE_PRESSURE_DIVISOR;
}

// Whether this thread's aggregate states should be exported to spillable storage
bool ShouldExportStates(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate, GroupedAggregateHashTable &ht) {
	const auto arena_size = ht.GetAggregateAllocator()->AllocationSize();
	if (arena_size == 0) {
		// every current state is inline, and exporting would only cost
		return false;
	}
	if (gstate.StatePressureExceeded(ht) || !lstate.abandoned_exported_data.empty()) {
		return true;
	}
	if (!gstate.external) {
		return false;
	}
	return arena_size >= gstate.GetThreadLimit() / RadixHTConfig::AGGREGATE_STATE_SPILL_DIVISOR;
}

void UpdateSinkReservation(ClientContext &context, RadixHTGlobalSinkState &gstate, GroupedAggregateHashTable &ht,
                           idx_t total_size) {
	// Include the headroom required by the arena's export threshold.
	const auto required_size = gstate.spill_plan
	                               ? MaxValue(total_size, ht.GetAggregateAllocator()->AllocationSize() *
	                                                          RadixHTConfig::AGGREGATE_STATE_PRESSURE_DIVISOR)
	                               : total_size;
	if (required_size <= gstate.GetThreadLimit() || gstate.external) {
		return;
	}
	const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
	if (required_size <= gstate.GetThreadLimit() || gstate.external) {
		return;
	}
	auto &temporary_memory_state = *gstate.temporary_memory_state;
	const auto aggregate_allocator_size = ht.GetAggregateAllocator()->AllocationSize();
	// Account for tuple metadata and the headroom deducted by GetThreadLimit.
	const auto minimum_reservation =
	    (gstate.minimum_reservation + gstate.number_of_threads * gstate.block_alloc_size) / 8 * 10;
	temporary_memory_state.SetMinimumReservation(aggregate_allocator_size * gstate.number_of_threads +
	                                             minimum_reservation);
	// Grow from current demand, not the previous request, which might not have been granted.
	const auto remaining_size =
	    MaxValue<idx_t>(2 * gstate.number_of_threads * required_size, temporary_memory_state.GetRemainingSize());
	temporary_memory_state.SetRemainingSizeAndUpdateReservation(context, remaining_size);
}

void MaybeRepartition(ClientContext &context, RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate,
                      const bool combine) {
	auto &config = gstate.config;
	auto &ht = *lstate.ht;
	const auto total_size = ht.GetSizeInBytes();
	UpdateSinkReservation(context, gstate, ht, total_size);

	if (total_size > gstate.GetThreadLimit()) {
		if (gstate.config.SetRadixBitsToExternal()) {
			lstate.PrepareForSpill(gstate);
			// We're approaching the memory limit, unpin the data
			const auto external_radix_bits = config.GetRadixBits();
			if (!lstate.abandoned_data) {
				lstate.abandoned_data = make_uniq<RadixPartitionedTupleData>(
				    BufferManager::GetBufferManager(context), gstate.radix_ht.GetLayoutPtr(), MemoryTag::HASH_TABLE,
				    external_radix_bits, gstate.radix_ht.GetLayout().ColumnCount() - 1, context);
			} else {
				GrowAbandonedDataToRadixBits(context, gstate, lstate, external_radix_bits);
			}
			ht.SetRadixBits(external_radix_bits);
			ht.AcquirePartitionedData()->Repartition(context, *lstate.abandoned_data);
		}
	}

	// We can go external when there are few threads, but we shouldn't repartition here
	if (!combine && gstate.number_of_threads <= RadixHTConfig::GROW_STRATEGY_THREAD_THRESHOLD) {
		return;
	}

	const auto partition_count = ht.GetPartitionedData().PartitionCount();
	const auto current_radix_bits = RadixPartitioning::RadixBitsOfPowerOfTwo(partition_count);
	D_ASSERT(current_radix_bits <= config.GetRadixBits());

	const auto row_size_per_partition =
	    ht.GetMaterializedCount() * ht.GetPartitionedData().GetLayout().GetRowWidth() / partition_count;
	if (row_size_per_partition >
	    LossyNumericCast<idx_t>(config.BLOCK_FILL_FACTOR * static_cast<double>(gstate.block_alloc_size))) {
		// We crossed our block filling threshold, try to increment radix bits
		config.SetRadixBits(current_radix_bits + config.REPARTITION_RADIX_BITS);
	}

	const auto global_radix_bits = config.GetRadixBits();
	if (current_radix_bits == global_radix_bits) {
		return; // We're already on the right number of radix bits
	}

	// We're out-of-sync with the global radix bits, repartition
	ht.SetRadixBits(global_radix_bits);
	ht.Repartition();
}

void RadixPartitionedHashTable::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input,
                                     DataChunk &payload_input, const unsafe_vector<idx_t> &filter) const {
	auto &gstate = input.global_state.Cast<RadixHTGlobalSinkState>();
	auto &lstate = input.local_state.Cast<RadixHTLocalSinkState>();
	if (!lstate.ht) {
		lstate.local_sink_capacity = gstate.config.sink_capacity;
		lstate.ht = CreateHT(context.client, lstate.local_sink_capacity, gstate.config.GetRadixBits());
		if (gstate.number_of_threads > RadixHTConfig::GROW_STRATEGY_THREAD_THRESHOLD) {
			lstate.ht->EnableHLL(true);
		}
	}
	if (!lstate.registered) {
		gstate.active_threads++;
		lstate.registered = true;
	}

	auto &group_chunk = lstate.group_chunk;
	PopulateGroupChunk(group_chunk, chunk);

	auto &ht = *lstate.ht;
	if (gstate.external && !lstate.spilling) {
		lstate.PrepareForSpill(gstate);
	}
	ht.AddChunk(group_chunk, payload_input, filter);

	// Row width and aggregate state size can increase without filling the pointer table.
	const auto total_size = ht.GetSizeInBytes();
	UpdateSinkReservation(context.client, gstate, ht, total_size);
	const auto memory_pressure =
	    total_size > gstate.GetThreadLimit() || (gstate.spill_plan && gstate.StatePressureExceeded(ht));
	if (!memory_pressure &&
	    ht.Count() + STANDARD_VECTOR_SIZE < GroupedAggregateHashTable::ResizeThreshold(lstate.local_sink_capacity)) {
		return;
	}

	if (!memory_pressure) {
		if (RadixHTAdaptivity::TryGrow(gstate, lstate)) {
			return;
		}
		RadixHTAdaptivity::MaybeSkipLookups(gstate, lstate);
	}

	if (gstate.number_of_threads > RadixHTConfig::GROW_STRATEGY_THREAD_THRESHOLD || gstate.external) {
		// Keep the materialized rows and clear only the local pointer table
		// We don't do this when running with 1 or 2 threads, it only makes sense when there's many threads
		gstate.any_abandoned = true;
		ht.Abandon();
		if (!memory_pressure) {
			RadixHTAdaptivity::MaybeResumeLookups(gstate, lstate);
		}
	}

	// Check if we need to repartition
	const auto radix_bits_before = ht.GetRadixBits();
	MaybeRepartition(context.client, gstate, lstate, false);
	const auto repartitioned = radix_bits_before != ht.GetRadixBits();

	if (repartitioned && ht.Count() != 0) {
		// We repartitioned, but we didn't clear the pointer table / reset the count because we're on 1 or 2 threads
		gstate.any_abandoned = true;
		ht.Abandon();
		if (gstate.external) {
			ht.Resize(lstate.local_sink_capacity);
		}
	}

	if (gstate.spill_plan && ShouldExportStates(gstate, lstate, ht)) {
		// Move everything the HT has abandoned out of it, export the states, and free the arena,
		// so that the state payloads can spill with the rest of the data
		if (!gstate.external) {
			// State pressure also makes the sink external: the finalize phase processes one
			// partition at a time, so the partitions must shrink to fit their states in memory
			gstate.config.SetRadixBitsToExternal();
			ht.SetRadixBits(gstate.config.GetRadixBits());
		}
		gstate.any_abandoned = true;
		ht.Abandon();
		if (!lstate.abandoned_data) {
			lstate.abandoned_data = make_uniq<RadixPartitionedTupleData>(
			    BufferManager::GetBufferManager(context.client), gstate.radix_ht.GetLayoutPtr(), MemoryTag::HASH_TABLE,
			    ht.GetRadixBits(), gstate.radix_ht.GetLayout().ColumnCount() - 1, context.client);
		} else {
			GrowAbandonedDataToRadixBits(context.client, gstate, lstate, ht.GetRadixBits());
		}
		lstate.PrepareForSpill(gstate);
		ht.AcquirePartitionedData()->Repartition(context.client, *lstate.abandoned_data);
		ExportAbandonedData(context.client, gstate, lstate);
		ht.GetAggregateAllocator()->FreeAll();
	}

	// TODO: combine early and often
}

void RadixPartitionedHashTable::Combine(ExecutionContext &context, GlobalSinkState &gstate_p,
                                        LocalSinkState &lstate_p) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();
	auto &lstate = lstate_p.Cast<RadixHTLocalSinkState>();
	if (!lstate.ht) {
		return;
	}

	// Set any_combined, then check one last time whether we need to repartition
	gstate.any_combined = true;
	MaybeRepartition(context.client, gstate, lstate, true);

	auto &ht = *lstate.ht;
	auto lstate_data = ht.AcquirePartitionedData();
	if (lstate.abandoned_data) {
		// Data is abandoned when the sink goes external, and also when aggregate state pressure
		// forces the states to be exported without the sink being external
		D_ASSERT(gstate.external || gstate.spill_plan);
		// The global radix bits may have grown after we last sized abandoned_data - grow it to match before combining
		GrowAbandonedDataToRadixBits(context.client, gstate, lstate, gstate.config.GetRadixBits());
		D_ASSERT(lstate.abandoned_data->PartitionCount() == lstate.ht->GetPartitionedData().PartitionCount());
		D_ASSERT(lstate.abandoned_data->PartitionCount() ==
		         RadixPartitioning::NumberOfPartitions(gstate.config.GetRadixBits()));
		lstate.abandoned_data->Combine(*lstate_data);
	} else {
		lstate.abandoned_data = std::move(lstate_data);
	}

	bool must_export = false;
	if (gstate.spill_plan) {
		// The decision between combining rows natively and exporting them must be synchronized
		// with the exported width: after it grew beyond the native width, every combine must
		// export, and conversely a native combine pins the exported width for good
		const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
		if (gstate.spill_phase == SpillPhase::EXPORTED_ONLY) {
			must_export = true;
		} else {
			gstate.spill_phase = SpillPhase::NATIVE_COMBINE_STARTED;
		}
	}
	if (gstate.spill_plan && (must_export || ShouldExportStates(gstate, lstate, ht))) {
		// Everything this thread produced is in abandoned_data now, export it and free the arena
		ExportAbandonedData(context.client, gstate, lstate);
		ht.GetAggregateAllocator()->FreeAll();
	}

	auto aggregate_allocator = ht.GetAggregateAllocator();

	const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
	D_ASSERT(!gstate.finalized);
	if (gstate.uncombined_data) {
		gstate.uncombined_data->Combine(*lstate.abandoned_data);
	} else {
		gstate.uncombined_data = std::move(lstate.abandoned_data);
	}
	if (!lstate.abandoned_exported_data.empty()) {
		if (gstate.uncombined_exported_data.empty()) {
			gstate.uncombined_exported_data = std::move(lstate.abandoned_exported_data);
		} else {
			if (gstate.uncombined_exported_data.size() < lstate.abandoned_exported_data.size()) {
				GrowExportedData(context.client, gstate, gstate.uncombined_exported_data,
				                 lstate.abandoned_exported_data.size());
			} else if (lstate.abandoned_exported_data.size() < gstate.uncombined_exported_data.size()) {
				GrowExportedData(context.client, gstate, lstate.abandoned_exported_data,
				                 gstate.uncombined_exported_data.size());
			}
			for (idx_t i = 0; i < gstate.uncombined_exported_data.size(); i++) {
				if (!lstate.abandoned_exported_data[i]) {
					continue;
				}
				if (gstate.uncombined_exported_data[i]) {
					gstate.uncombined_exported_data[i]->Combine(*lstate.abandoned_exported_data[i]);
				} else {
					gstate.uncombined_exported_data[i] = std::move(lstate.abandoned_exported_data[i]);
				}
			}
			lstate.abandoned_exported_data.clear();
		}
	}
	if (aggregate_allocator->AllocationSize() != 0) {
		gstate.stored_allocators.emplace_back(std::move(aggregate_allocator));
		gstate.stored_allocators_size += gstate.stored_allocators.back()->AllocationSize();
	}
}

void RadixPartitionedHashTable::Finalize(ClientContext &context, GlobalSinkState &gstate_p) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();
	const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
	D_ASSERT(!gstate.finalized);

	if (!gstate.uncombined_exported_data.empty() &&
	    (!gstate.uncombined_data ||
	     gstate.uncombined_exported_data.size() != gstate.uncombined_data->PartitionCount())) {
		// The exported side grew to more partitions than the native side, which only happens when
		// state pressure made every thread drain: all rows travel in the exported form
		D_ASSERT(gstate.spill_phase == SpillPhase::EXPORTED_ONLY);
		D_ASSERT(!gstate.uncombined_data || gstate.uncombined_data->Count() == 0);
		gstate.count_before_combining = 0;
		const auto n_partitions = gstate.uncombined_exported_data.size();
		gstate.partitions.reserve(n_partitions);
		for (idx_t i = 0; i < n_partitions; i++) {
			auto empty_data =
			    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), gstate.radix_ht.GetLayoutPtr(),
			                                   MemoryTag::HASH_TABLE, nullptr, context);
			gstate.partitions.emplace_back(make_uniq<AggregatePartition>(std::move(empty_data)));
			auto &partition = *gstate.partitions.back();
			partition.exported_data = std::move(gstate.uncombined_exported_data[i]);
			if (partition.exported_data) {
				gstate.count_before_combining += partition.exported_data->Count();
				gstate.max_partition_size =
				    MaxValue(gstate.max_partition_size,
				             RadixHTConfig::EXPORTED_STATE_MEMORY_MULTIPLIER * partition.exported_data->SizeInBytes());
			}
		}
		gstate.uncombined_exported_data.clear();
	} else if (gstate.uncombined_data) {
		auto &uncombined_data = *gstate.uncombined_data;
		gstate.count_before_combining = uncombined_data.Count();

		// If true there is no need to combine, it was all done by a single thread in a single HT.
		// This is the case when only one thread contributed data and the HT never overflowed its
		// capacity (which would have caused Abandon() to be called, creating duplicates).
		const auto single_ht = !gstate.external && gstate.active_threads == 1 && !gstate.any_abandoned;

		auto &uncombined_partition_data = uncombined_data.GetPartitions();
		const auto n_partitions = uncombined_partition_data.size();
		gstate.partitions.reserve(n_partitions);
		for (idx_t i = 0; i < n_partitions; i++) {
			auto &partition = uncombined_partition_data[i];
			auto partition_size =
			    partition->SizeInBytes() +
			    GroupedAggregateHashTable::GetCapacityForCount(partition->Count()) * sizeof(ht_entry_t);

			gstate.partitions.emplace_back(make_uniq<AggregatePartition>(std::move(partition)));
			if (!gstate.uncombined_exported_data.empty()) {
				D_ASSERT(gstate.uncombined_exported_data.size() == n_partitions);
				gstate.partitions.back()->exported_data = std::move(gstate.uncombined_exported_data[i]);
				if (gstate.partitions.back()->exported_data) {
					gstate.count_before_combining += gstate.partitions.back()->exported_data->Count();
					partition_size += RadixHTConfig::EXPORTED_STATE_MEMORY_MULTIPLIER *
					                  gstate.partitions.back()->exported_data->SizeInBytes();
				}
			}
			gstate.max_partition_size = MaxValue(gstate.max_partition_size, partition_size);
			if (single_ht) {
				gstate.finalize_done++;
				gstate.partitions.back()->finalized = true;
				gstate.partitions.back()->state = AggregatePartitionState::READY_TO_SCAN;
			}
		}
	} else {
		gstate.count_before_combining = 0;
	}

	// Minimum of combining one partition at a time
	gstate.temporary_memory_state->SetMinimumReservation(gstate.stored_allocators_size + gstate.max_partition_size);
	// Set size to 0 until the scan actually starts
	gstate.temporary_memory_state->SetZero();
	gstate.finalized = true;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
idx_t RadixPartitionedHashTable::MaxThreads(GlobalSinkState &sink_p) const {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	if (sink.partitions.empty()) {
		return 0;
	}

	const auto max_threads =
	    MinValue<idx_t>(TaskScheduler::GetScheduler(sink.context).NumberOfThreads(), sink.partitions.size());
	sink.temporary_memory_state->SetRemainingSizeAndUpdateReservation(
	    sink.context, sink.stored_allocators_size + max_threads * sink.max_partition_size);

	// we cannot spill aggregate state memory
	const auto usable_memory = sink.temporary_memory_state->GetReservation() > sink.stored_allocators_size
	                               ? sink.temporary_memory_state->GetReservation() - sink.stored_allocators_size
	                               : 0;
	// This many partitions will fit given our reservation (at least 1))
	const auto partitions_fit = MaxValue<idx_t>(usable_memory / sink.max_partition_size, 1);

	// Minimum of the two
	return MinValue<idx_t>(partitions_fit, max_threads);
}

void RadixPartitionedHashTable::SetMultiScan(GlobalSinkState &sink_p) {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	sink.scan_pin_properties = TupleDataPinProperties::UNPIN_AFTER_DONE;
}

enum class RadixHTSourceTaskType : uint8_t { NO_TASK, FINALIZE, SCAN };

class RadixHTLocalSourceState;

class RadixHTGlobalSourceState : public GlobalSourceState {
public:
	RadixHTGlobalSourceState(ClientContext &context, const RadixPartitionedHashTable &radix_ht);

	//! Assigns a task to a local source state
	SourceResultType AssignTask(RadixHTGlobalSinkState &sink, RadixHTLocalSourceState &lstate,
	                            InterruptState &interrupt_state);

public:
	//! The client context
	ClientContext &context;
	//! For synchronizing the source phase
	atomic<bool> finished;

	//! Column ids for scanning
	vector<column_t> column_ids;

	//! For synchronizing tasks
	atomic<idx_t> task_idx;
	atomic<idx_t> task_done;
	//! Scan progress: the partitions whose scan started, the rows in these partitions, and the rows scanned
	atomic<idx_t> started_partitions;
	atomic<idx_t> started_rows;
	atomic<idx_t> scanned_rows;
	//! The scan progress estimate drops when a larger partition starts scanning
	MonotonicProgress scan_progress;
};

enum class RadixHTScanStatus : uint8_t { INIT, IN_PROGRESS, DONE };

class RadixHTLocalSourceState : public LocalSourceState {
public:
	explicit RadixHTLocalSourceState(ExecutionContext &context, const RadixPartitionedHashTable &radix_ht);
	void ResetForReuse();

public:
	//! Do the work this thread has been assigned
	void ExecuteTask(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate, DataChunk &chunk);
	//! Whether this thread has finished the work it has been assigned
	bool TaskFinished();

private:
	//! Execute the finalize or scan task
	void Finalize(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate);
	void Scan(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate, DataChunk &chunk);

public:
	//! Current task and index
	RadixHTSourceTaskType task;
	idx_t task_idx;

	//! Thread-local HT that is re-used to Finalize
	unique_ptr<GroupedAggregateHashTable> ht;
	//! Current status of a Scan
	RadixHTScanStatus scan_status;

private:
	//! Allocator and layout for finalizing state
	TupleDataLayout layout;
	ArenaAllocator aggregate_allocator;
	RowOperationsState row_state;

	//! State and chunk for scanning
	TupleDataScanState scan_state;
	DataChunk scan_chunk;
};

unique_ptr<GlobalSourceState> RadixPartitionedHashTable::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<RadixHTGlobalSourceState>(context, *this);
}

unique_ptr<LocalSourceState> RadixPartitionedHashTable::GetLocalSourceState(ExecutionContext &context) const {
	return make_uniq<RadixHTLocalSourceState>(context, *this);
}

void RadixPartitionedHashTable::ResetGlobalSourceState(ClientContext &context, GlobalSourceState &gstate_p) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSourceState>();
	gstate.finished = false;
	gstate.task_idx = 0;
	gstate.task_done = 0;
	gstate.started_partitions = 0;
	gstate.started_rows = 0;
	gstate.scanned_rows = 0;
	gstate.scan_progress.Reset();
}

RadixHTGlobalSourceState::RadixHTGlobalSourceState(ClientContext &context_p, const RadixPartitionedHashTable &radix_ht)
    : context(context_p), finished(false), task_idx(0), task_done(0), started_partitions(0), started_rows(0),
      scanned_rows(0) {
	for (column_t column_id = 0; column_id < radix_ht.group_types.size(); column_id++) {
		column_ids.push_back(column_id);
	}
}

SourceResultType RadixHTGlobalSourceState::AssignTask(RadixHTGlobalSinkState &sink, RadixHTLocalSourceState &lstate,
                                                      InterruptState &interrupt_state) {
	// First, try to get a partition index
	lstate.task_idx = task_idx++;
	if (finished || lstate.task_idx >= sink.partitions.size()) {
		lstate.ht.reset();
		return SourceResultType::FINISHED;
	}

	// We got a partition index
	auto &partition = *sink.partitions[lstate.task_idx];
	const annotated_lock_guard<annotated_mutex> partition_guard {partition.lock};
	switch (partition.state) {
	case AggregatePartitionState::READY_TO_FINALIZE:
		partition.state = AggregatePartitionState::FINALIZE_IN_PROGRESS;
		lstate.task = RadixHTSourceTaskType::FINALIZE;
		return SourceResultType::HAVE_MORE_OUTPUT;
	case AggregatePartitionState::FINALIZE_IN_PROGRESS:
		lstate.task = RadixHTSourceTaskType::SCAN;
		lstate.scan_status = RadixHTScanStatus::INIT;
		return partition.BlockSource(interrupt_state);
	case AggregatePartitionState::READY_TO_SCAN:
		lstate.task = RadixHTSourceTaskType::SCAN;
		lstate.scan_status = RadixHTScanStatus::INIT;
		return SourceResultType::HAVE_MORE_OUTPUT;
	default:
		throw InternalException("Unexpected AggregatePartitionState in RadixHTLocalSourceState::Finalize!");
	}
}

RadixHTLocalSourceState::RadixHTLocalSourceState(ExecutionContext &context, const RadixPartitionedHashTable &radix_ht)
    : layout(radix_ht.GetLayout().Copy()), aggregate_allocator(BufferAllocator::Get(context.client)),
      row_state(aggregate_allocator) {
	auto &allocator = BufferAllocator::Get(context.client);
	auto scan_chunk_types = radix_ht.group_types;
	for (auto &aggr_type : radix_ht.op.aggregate_return_types) {
		scan_chunk_types.push_back(aggr_type);
	}
	scan_chunk.Initialize(allocator, scan_chunk_types);
	ResetForReuse();
}

void RadixHTLocalSourceState::ResetForReuse() {
	task = RadixHTSourceTaskType::NO_TASK;
	task_idx = DConstants::INVALID_INDEX;
	ht.reset();
	scan_status = RadixHTScanStatus::DONE;
	aggregate_allocator.Reset();
	row_state.addresses.reset();
	scan_state.Reset();
	scan_chunk.Reset();
}

void RadixPartitionedHashTable::ResetLocalSourceState(ExecutionContext &context, LocalSourceState &lstate_p) const {
	auto &lstate = lstate_p.Cast<RadixHTLocalSourceState>();
	lstate.ResetForReuse();
}

void RadixHTLocalSourceState::ExecuteTask(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate,
                                          DataChunk &chunk) {
	D_ASSERT(task != RadixHTSourceTaskType::NO_TASK);
	switch (task) {
	case RadixHTSourceTaskType::FINALIZE:
		Finalize(sink, gstate);
		break;
	case RadixHTSourceTaskType::SCAN:
		Scan(sink, gstate, chunk);
		break;
	default:
		throw InternalException("Unexpected RadixHTSourceTaskType in ExecuteTask!");
	}
}

void RadixHTLocalSourceState::Finalize(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate) {
	D_ASSERT(task == RadixHTSourceTaskType::FINALIZE);
	D_ASSERT(scan_status != RadixHTScanStatus::IN_PROGRESS);
	auto &partition = *sink.partitions[task_idx];

	// When a partition holds exported states, it gets a fresh HT so that its arena can be
	// released as soon as the partition has been scanned
	const auto imports_states = partition.exported_data != nullptr;
	const auto fresh_ht = !ht || imports_states;
	if (fresh_ht) {
		// This capacity would always be sufficient for all data
		const auto exported_count = partition.exported_data ? partition.exported_data->Count() : 0;
		const auto capacity = GroupedAggregateHashTable::GetCapacityForCount(partition.data->Count() + exported_count);

		// However, we will limit the initial capacity so we don't do a huge over-allocation
		const auto n_threads = TaskScheduler::GetScheduler(gstate.context).NumberOfThreads();
		const auto memory_limit = BufferManager::GetBufferManager(gstate.context).GetMaxMemory();
		const idx_t thread_limit = LossyNumericCast<idx_t>(0.6 * double(memory_limit) / double(n_threads));

		const idx_t size_per_entry = partition.data->SizeInBytes() / MaxValue<idx_t>(partition.data->Count(), 1) +
		                             idx_t(GroupedAggregateHashTable::LOAD_FACTOR * sizeof(ht_entry_t));
		// but not lower than the initial capacity
		const auto capacity_limit =
		    MaxValue(NextPowerOfTwo(thread_limit / size_per_entry), GroupedAggregateHashTable::InitialCapacity());

		ht = sink.radix_ht.CreateHT(gstate.context, MinValue<idx_t>(capacity, capacity_limit), 0);
	} else {
		ht->Abandon();
	}

	// Now combine the uncombined data using this thread's HT
	partition.combine_chunk_count = partition.data->ChunkCount();
	ht->Combine(*partition.data, &partition.combined_chunks);
	if (partition.exported_data) {
		// Rebuild the exported states on an arena of their own, one chunk at a time, and combine
		// them like the rest. Combining may steal from the imported states, so the arena lives
		// until the scan is done.
		partition.import_allocator = make_shared_ptr<ArenaAllocator>(BufferAllocator::Get(gstate.context));
		AggregateStateSpilling::ImportStates(gstate.context, sink.radix_ht.GetLayoutPtr(), *sink.spill_plan,
		                                     *partition.exported_data, *partition.import_allocator,
		                                     [&](TupleDataCollection &imported) { ht->Combine(imported); });
		partition.exported_data.reset();
	}
	partition.finalized = true;

	// Move the combined data back to the partition
	partition.data =
	    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(gstate.context), sink.radix_ht.GetLayoutPtr(),
	                                   MemoryTag::HASH_TABLE, nullptr, gstate.context);
	partition.data->Combine(*ht->AcquirePartitionedData()->GetPartitions()[0]);

	// Update thread-global state
	const annotated_lock_guard<annotated_mutex> guard {sink.lock};
	if (imports_states) {
		// The arena only holds this partition's states, release it once the partition is scanned
		partition.allocator = ht->GetAggregateAllocator();
	} else {
		sink.stored_allocators.emplace_back(ht->GetAggregateAllocator());
	}
	if (task_idx == sink.partitions.size()) {
		ht.reset();
	}
	const auto finalizes_done = ++sink.finalize_done;
	D_ASSERT(finalizes_done <= sink.partitions.size());
	if (finalizes_done == sink.partitions.size()) {
		// All finalizes are done, set remaining size to 0
		sink.temporary_memory_state->SetZero();
	}

	// Update partition state
	const annotated_lock_guard<annotated_mutex> partition_guard {partition.lock};
	partition.state = AggregatePartitionState::READY_TO_SCAN;
	partition.UnblockTasks();

	// This thread will scan the partition
	task = RadixHTSourceTaskType::SCAN;
	scan_status = RadixHTScanStatus::INIT;
}

void RadixHTLocalSourceState::Scan(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate, DataChunk &chunk) {
	D_ASSERT(task == RadixHTSourceTaskType::SCAN);
	D_ASSERT(scan_status != RadixHTScanStatus::DONE);

	auto &partition = *sink.partitions[task_idx];
	D_ASSERT(partition.state == AggregatePartitionState::READY_TO_SCAN);
	auto &data_collection = *partition.data;

	if (scan_status == RadixHTScanStatus::INIT) {
		data_collection.InitializeScan(scan_state, gstate.column_ids, sink.scan_pin_properties);
		scan_status = RadixHTScanStatus::IN_PROGRESS;
		gstate.started_rows.fetch_add(data_collection.Count(), std::memory_order_relaxed);
		gstate.started_partitions.fetch_add(1, std::memory_order_relaxed);
	}

	if (!data_collection.Scan(scan_state, scan_chunk)) {
		if (sink.scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE) {
			data_collection.Reset();
			partition.allocator.reset();
			partition.import_allocator.reset();
		}
		scan_status = RadixHTScanStatus::DONE;
		const annotated_lock_guard<annotated_mutex> guard {sink.lock};
		if (++gstate.task_done == sink.partitions.size()) {
			gstate.finished = true;
		}
		return;
	}

	gstate.scanned_rows.fetch_add(scan_chunk.size(), std::memory_order_relaxed);

	const auto group_cols = layout.ColumnCount() - 1;
	RowOperations::FinalizeStates(row_state, layout, scan_state.chunk_state.row_locations, scan_chunk, group_cols);

	if (sink.scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE && layout.HasDestructor()) {
		RowOperations::DestroyStates(row_state, layout, scan_state.chunk_state.row_locations);
	}

	auto &radix_ht = sink.radix_ht;
	idx_t chunk_index = 0;
	for (auto &entry : radix_ht.grouping_set) {
		chunk.data[entry].Reference(scan_chunk.data[chunk_index++]);
	}
	for (auto null_group : radix_ht.null_groups) {
		ConstantVector::SetNull(chunk.data[null_group], count_t(scan_chunk.size()));
	}
	D_ASSERT(radix_ht.grouping_set.size() + radix_ht.null_groups.size() == radix_ht.op.GroupCount());
	for (idx_t col_idx = 0; col_idx < radix_ht.op.aggregates.size(); col_idx++) {
		chunk.data[radix_ht.op.GroupCount() + col_idx].Reference(
		    scan_chunk.data[radix_ht.group_types.size() + col_idx]);
	}
	D_ASSERT(radix_ht.op.grouping_functions.size() == radix_ht.grouping_values.size());
	for (idx_t i = 0; i < radix_ht.op.grouping_functions.size(); i++) {
		chunk.data[radix_ht.op.GroupCount() + radix_ht.op.aggregates.size() + i].Reference(radix_ht.grouping_values[i],
		                                                                                   count_t(scan_chunk.size()));
	}
	D_ASSERT(chunk.size() != 0);
}

bool RadixHTLocalSourceState::TaskFinished() {
	switch (task) {
	case RadixHTSourceTaskType::FINALIZE:
		return true;
	case RadixHTSourceTaskType::SCAN:
		return scan_status == RadixHTScanStatus::DONE;
	default:
		D_ASSERT(task == RadixHTSourceTaskType::NO_TASK);
		return true;
	}
}

SourceResultType RadixPartitionedHashTable::GetData(ExecutionContext &context, DataChunk &chunk,
                                                    GlobalSinkState &sink_p, OperatorSourceInput &input) const {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	D_ASSERT(sink.finalized);

	auto &gstate = input.global_state.Cast<RadixHTGlobalSourceState>();
	auto &lstate = input.local_state.Cast<RadixHTLocalSourceState>();
	D_ASSERT(sink.scan_pin_properties == TupleDataPinProperties::UNPIN_AFTER_DONE ||
	         sink.scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE);

	if (gstate.finished) {
		lstate.ht.reset();
		return SourceResultType::FINISHED;
	}

	if (sink.count_before_combining == 0) {
		if (grouping_set.empty()) {
			// Special case hack to sort out aggregating from empty intermediates for aggregations without groups
			D_ASSERT(chunk.ColumnCount() == null_groups.size() + op.aggregates.size() + op.grouping_functions.size());
			// For each column in the aggregates, set to initial state
			chunk.SetChildCardinality(1);
			for (auto null_group : null_groups) {
				ConstantVector::SetNull(chunk.data[null_group], count_t(1));
			}
			ArenaAllocator allocator(BufferAllocator::Get(context.client));
			for (idx_t i = 0; i < op.aggregates.size(); i++) {
				D_ASSERT(op.aggregates[i]->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
				auto &aggr = op.aggregates[i]->Cast<BoundAggregateExpression>();
				AggregateStateInput state_input(aggr.Function(), aggr.BindInfo().get());
				auto aggr_state =
				    make_unsafe_uniq_array_uninitialized<data_t>(aggr.Function().GetStateSizeCallback()(state_input));
				data_ptr_t state_ptr = aggr_state.get();
				aggr.Function().GetStateInitCallback()(state_input, &state_ptr, 1);

				AggregateFinalizeInputData aggr_input_data(aggr, allocator);
				Vector state_vector(Value::POINTER(CastPointerToValue(aggr_state.get())), count_t(1));
				auto &agg_result = chunk.data[null_groups.size() + i];
				aggr.Function().GetStateFinalizeCallback()(state_vector, aggr_input_data, agg_result, 1, 0);
				FlatVector::SetSize(agg_result, count_t(1));
				if (aggr.Function().HasStateDestructorCallback()) {
					aggr.Function().GetStateDestructorCallback()(state_vector, aggr_input_data, 1);
				}
			}
			// Place the grouping values (all the groups of the grouping_set condensed into a single value)
			// Behind the null groups + aggregates
			for (idx_t i = 0; i < op.grouping_functions.size(); i++) {
				chunk.data[null_groups.size() + op.aggregates.size() + i].Reference(grouping_values[i], count_t(1));
			}
		}
		gstate.finished = true;
		return SourceResultType::FINISHED;
	}

	while (!gstate.finished && chunk.size() == 0) {
		if (lstate.TaskFinished()) {
			const auto res = gstate.AssignTask(sink, lstate, input.interrupt_state);
			if (res != SourceResultType::HAVE_MORE_OUTPUT) {
				D_ASSERT(res == SourceResultType::FINISHED || res == SourceResultType::BLOCKED);
				return res;
			}
		}
		lstate.ExecuteTask(sink, gstate, chunk);
	}

	if (chunk.size() != 0) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	} else {
		lstate.ht.reset();
		return SourceResultType::FINISHED;
	}
}

ProgressData RadixPartitionedHashTable::GetProgress(ClientContext &, GlobalSinkState &sink_p,
                                                    GlobalSourceState &gstate_p) const {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	auto &gstate = gstate_p.Cast<RadixHTGlobalSourceState>();

	// Get partition combine progress, weigh it 2x
	ProgressData progress;
	for (auto &partition : sink.partitions) {
		progress.done += 2.0 * partition->GetCombineProgress();
	}

	// Get scan progress in partitions, weigh it 1x - weigh the partitions whose scan started by the scanned rows
	const auto partition_count = static_cast<double>(sink.partitions.size());
	const auto started_partitions = static_cast<double>(gstate.started_partitions.load(std::memory_order_relaxed));
	const auto started_rows = gstate.started_rows.load(std::memory_order_relaxed);
	const auto scanned_rows = gstate.scanned_rows.load(std::memory_order_relaxed);
	double scanned_partitions = started_partitions;
	if (started_rows > 0) {
		scanned_partitions *=
		    static_cast<double>(MinValue(scanned_rows, started_rows)) / static_cast<double>(started_rows);
	}
	auto scan_progress =
	    gstate.scan_progress.Update(ProgressData {scanned_partitions, MaxValue(partition_count, 1.0), false});
	progress.done += 1.0 * scan_progress.done;

	// Divide by 3x for the weights, and the number of partitions to get a value between 0 and 1 again
	progress.total += 3.0 * partition_count;

	return progress;
}

} // namespace duckdb
