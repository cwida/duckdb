#include "duckdb/execution/aggregate_hashtable.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/atomic.hpp"
namespace duckdb {

using ValidityBytes = TupleDataLayout::ValidityBytes;

GroupedAggregateHashTable::GroupedAggregateHashTable(ClientContext &context, Allocator &allocator,
                                                     vector<LogicalType> group_types, vector<LogicalType> payload_types,
                                                     const vector<BoundAggregateExpression *> &bindings,
                                                     idx_t initial_capacity, idx_t radix_bits,
                                                     TupleDataValidityType group_validity)
    : GroupedAggregateHashTable(context, allocator, std::move(group_types), std::move(payload_types),
                                AggregateObject::CreateAggregateObjects(bindings), initial_capacity, radix_bits,
                                group_validity) {
}

GroupedAggregateHashTable::GroupedAggregateHashTable(ClientContext &context, Allocator &allocator,
                                                     vector<LogicalType> group_types,
                                                     TupleDataValidityType group_validity)
    : GroupedAggregateHashTable(context, allocator, std::move(group_types), {}, vector<AggregateObject>(),
                                InitialCapacity(), 0, group_validity) {
}

GroupedAggregateHashTable::AggregateHTAppendState::AggregateHTAppendState(ArenaAllocator &allocator)
    : new_groups(STANDARD_VECTOR_SIZE), row_state(allocator) {
}

AggregateHTProbeState::AggregateHTProbeState()
    : hashes(LogicalType::HASH), ht_offsets(LogicalType::UBIGINT), hash_salts(LogicalType::HASH),
      addresses(LogicalType::POINTER), group_compare_vector(STANDARD_VECTOR_SIZE),
      no_match_vector(STANDARD_VECTOR_SIZE) {
}

AggregateHTLookupState::AggregateHTLookupState() : missing_vector(STANDARD_VECTOR_SIZE) {
}

GroupedAggregateHashTable::GroupedAggregateHashTable(ClientContext &context_p, Allocator &allocator,
                                                     vector<LogicalType> group_types_p,
                                                     vector<LogicalType> payload_types_p,
                                                     vector<AggregateObject> aggregate_objects_p,
                                                     idx_t initial_capacity, idx_t radix_bits,
                                                     TupleDataValidityType group_validity)
    : BaseAggregateHashTable(context_p, allocator, aggregate_objects_p, std::move(payload_types_p)), context(context_p),
      radix_bits(radix_bits), count(0), capacity(0), aggregate_allocator(make_shared_ptr<ArenaAllocator>(allocator)),
      state(*aggregate_allocator) {
	state.owner = this;
	state.initialized = true;
	clustered_state.all_clustered = AllAggregatesClustered(aggregate_objects_p);
	clustered_state.n_clustered = CountAggregatesClustered(aggregate_objects_p);
	if (clustered_state.n_clustered > 1) {
		clustered_state.Initialize();
	}

	// Append hash column to the end and initialise the row layout
	group_types_p.emplace_back(LogicalType::HASH);

	auto layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(std::move(group_types_p), std::move(aggregate_objects_p), group_validity);
	layout_ptr = std::move(layout);

	hash_offset = layout_ptr->GetOffsets()[layout_ptr->ColumnCount() - 1];

	// Partitioned data and pointer table
	InitializePartitionedData();
	if (radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD) {
		InitializeUnpartitionedData();
	}
	Resize(initial_capacity);

	// Predicates
	const auto expr_type = (layout_ptr->CannotHaveNull() && !layout_ptr->HasNestedTypes())
	                           ? ExpressionType::COMPARE_EQUAL
	                           : ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	predicates.resize(layout_ptr->ColumnCount() - 1, expr_type);
	row_matcher.Initialize(true, *layout_ptr, predicates);

	state.partitioned_append_state.compute_reverse_partition_sel = true;
	state.unpartitioned_append_state.compute_reverse_partition_sel = true;
}

void GroupedAggregateHashTable::InitializePartitionedData() {
	tuple_size_cache.count.SetInvalid();
	if (!partitioned_data ||
	    RadixPartitioning::RadixBitsOfPowerOfTwo(partitioned_data->PartitionCount()) != radix_bits) {
		D_ASSERT(!partitioned_data || partitioned_data->Count() == 0);
		partitioned_data = make_uniq<RadixPartitionedTupleData>(buffer_manager, layout_ptr, MemoryTag::HASH_TABLE,
		                                                        radix_bits, layout_ptr->ColumnCount() - 1, context);
	} else {
		partitioned_data->Reset();
	}

	D_ASSERT(GetLayout().GetAggrWidth() == layout_ptr->GetAggrWidth());
	D_ASSERT(GetLayout().GetDataWidth() == layout_ptr->GetDataWidth());
	D_ASSERT(GetLayout().GetRowWidth() == layout_ptr->GetRowWidth());

	partitioned_data->InitializeAppendState(state.partitioned_append_state,
	                                        TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
}

void GroupedAggregateHashTable::InitializeUnpartitionedData() {
	tuple_size_cache.count.SetInvalid();
	D_ASSERT(radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD);
	if (!unpartitioned_data) {
		unpartitioned_data = make_uniq<RadixPartitionedTupleData>(buffer_manager, layout_ptr, MemoryTag::HASH_TABLE,
		                                                          0ULL, layout_ptr->ColumnCount() - 1, context);
	} else {
		unpartitioned_data->Reset();
	}
	unpartitioned_data->InitializeAppendState(state.unpartitioned_append_state,
	                                          TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
}

const PartitionedTupleData &GroupedAggregateHashTable::GetPartitionedData() const {
	return *partitioned_data;
}

idx_t GroupedAggregateHashTable::GetSizeInBytes() const {
	const auto materialized_count = GetMaterializedCount();
	if (!tuple_size_cache.count.IsValid() || tuple_size_cache.count.GetIndex() != materialized_count) {
		tuple_size_cache.size = partitioned_data->GetAllocatedSizeInBytes() +
		                        (unpartitioned_data ? unpartitioned_data->GetAllocatedSizeInBytes() : 0);
		tuple_size_cache.count = materialized_count;
	}
	return tuple_size_cache.size + aggregate_allocator->AllocationSize() + hash_map.GetSize();
}

unique_ptr<PartitionedTupleData> GroupedAggregateHashTable::AcquirePartitionedData() {
	// Release destination pins before copying the buffered rows.
	partitioned_data->FlushAppendState(state.partitioned_append_state);
	partitioned_data->Unpin();
	if (radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD) {
		// Flush/unpin unpartitioned data and append to partitioned data
		if (unpartitioned_data) {
			unpartitioned_data->FlushAppendState(state.unpartitioned_append_state);
			unpartitioned_data->Unpin();
			unpartitioned_data->Repartition(context, *partitioned_data);
			// Release scratch metadata along with the rows transferred to partitioned_data.
			unpartitioned_data.reset();
		}
		InitializeUnpartitionedData();
	}

	// Return and re-initialize
	auto result = std::move(partitioned_data);
	InitializePartitionedData();
	return result;
}

void GroupedAggregateHashTable::Abandon() {
	if (radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD) {
		partitioned_data->FlushAppendState(state.partitioned_append_state);
		partitioned_data->Unpin();
		// Flush/unpin unpartitioned data and append to partitioned data
		if (unpartitioned_data) {
			unpartitioned_data->FlushAppendState(state.unpartitioned_append_state);
			unpartitioned_data->Unpin();
			unpartitioned_data->Repartition(context, *partitioned_data);
		}
		InitializeUnpartitionedData();
	}

	// Start over
	ClearPointerTable();
	count = 0;
	adaptivity.BeginCycle();

	// Resetting the id ensures the dict state is reset properly when needed
	state.dict_state.dictionary_id = string();
}

void GroupedAggregateHashTable::Repartition() {
	auto old = AcquirePartitionedData();
	D_ASSERT(old->GetPartitions().size() != partitioned_data->GetPartitions().size());
	old->Repartition(context, *partitioned_data);
}

shared_ptr<ArenaAllocator> GroupedAggregateHashTable::GetAggregateAllocator() {
	return aggregate_allocator;
}

GroupedAggregateHashTable::~GroupedAggregateHashTable() {
	// destroying the states pins their blocks, which can fail when the table is torn down after an out-of-memory
	// error - a destructor cannot throw, so the states leak in that case
	try {
		Destroy();
	} catch (...) { // NOLINT
	}
}

void GroupedAggregateHashTable::Destroy() {
	if (!layout_ptr->HasDestructor()) {
		return;
	}

	// There are aggregates with destructors
	if (unpartitioned_data) {
		DestroyAggregateData(*unpartitioned_data, state.unpartitioned_append_state);
	}
	if (partitioned_data) {
		DestroyAggregateData(*partitioned_data, state.partitioned_append_state);
	}
}

void GroupedAggregateHashTable::DestroyAggregateData(PartitionedTupleData &data,
                                                     PartitionedTupleDataAppendState &append_state) {
	// Call the destructor for each of the aggregates
	data.FlushAppendState(append_state);
	ErrorData first_error;
	for (auto &data_collection : data.GetPartitions()) {
		if (data_collection->Count() == 0) {
			continue;
		}
		try {
			TupleDataChunkIterator iterator(*data_collection, TupleDataPinProperties::DESTROY_AFTER_DONE, false);
			auto &row_locations = iterator.GetChunkState().row_locations;
			do {
				RowOperations::DestroyStates(state.row_state, *layout_ptr, row_locations);
			} while (iterator.Next());
		} catch (std::exception &ex) {
			// keep releasing the other partitions, freeing their memory may let their states be destroyed
			if (!first_error.HasError()) {
				first_error = ErrorData(ex);
			}
		}
		data_collection->Reset();
	}
	if (first_error.HasError()) {
		first_error.Throw();
	}
}

shared_ptr<TupleDataLayout> GroupedAggregateHashTable::GetLayoutPtr() {
	return partitioned_data->GetLayoutPtr();
}

const TupleDataLayout &GroupedAggregateHashTable::GetLayout() const {
	return partitioned_data->GetLayout();
}

idx_t GroupedAggregateHashTable::Count() const {
	return count;
}

idx_t GroupedAggregateHashTable::InitialCapacity() {
	return STANDARD_VECTOR_SIZE * 2ULL;
}

idx_t GroupedAggregateHashTable::GetCapacityForCount(idx_t count) {
	count = MaxValue<idx_t>(InitialCapacity(), count);
	return NextPowerOfTwo(LossyNumericCast<uint64_t>(static_cast<double>(count) * LOAD_FACTOR));
}

idx_t GroupedAggregateHashTable::Capacity() const {
	return capacity;
}

idx_t GroupedAggregateHashTable::ResizeThreshold() const {
	return ResizeThreshold(Capacity());
}

idx_t GroupedAggregateHashTable::ResizeThreshold(const idx_t capacity) {
	return LossyNumericCast<idx_t>(static_cast<double>(capacity) / LOAD_FACTOR);
}

idx_t GroupedAggregateHashTable::ApplyBitMask(hash_t hash) const {
	return hash & bitmask;
}

void GroupedAggregateHashTable::Verify() {
#ifdef DEBUG
	if (adaptivity.LookupsSkipped()) {
		return;
	}
	idx_t total_count = 0;
	for (idx_t i = 0; i < capacity; i++) {
		const auto &entry = entries[i];
		if (!entry.IsOccupied()) {
			continue;
		}
		auto hash = Load<hash_t>(entry.GetPointer() + hash_offset);
		D_ASSERT(entry.GetSalt() == ht_entry_t::ExtractSalt(hash));
		total_count++;
	}
	D_ASSERT(total_count == Count());
#endif
}

void GroupedAggregateHashTable::ClearPointerTable() {
	std::fill_n(entries, capacity, ht_entry_t());
}

void GroupedAggregateHashTable::SetRadixBits(idx_t radix_bits_p) {
	radix_bits = radix_bits_p;
}

idx_t GroupedAggregateHashTable::GetRadixBits() const {
	return radix_bits;
}

idx_t GroupedAggregateHashTable::GetMaterializedCount() const {
	auto result = partitioned_data->Count();
	if (unpartitioned_data) {
		result += unpartitioned_data->Count();
	}
	return result;
}

void GroupedAggregateHashTable::SkipLookups() {
	adaptivity.SkipLookups();
}

void GroupedAggregateHashTable::ResumeLookups() {
	D_ASSERT(Count() == 0);
	adaptivity.ResumeLookups();
}

void GroupedAggregateHashTable::EnableHLL(bool enable) {
	D_ASSERT(!enable || adaptivity.HLLEnabled() || GetMaterializedCount() == 0);
	adaptivity.EnableHLL(enable);
}

void GroupedAggregateHashTable::Resize(idx_t size) {
	D_ASSERT(IsPowerOfTwo(size));
	if (Count() != 0 && size < capacity) {
		throw InternalException("Cannot downsize a non-empty hash table!");
	}
	D_ASSERT(Count() == 0 || Count() == GetMaterializedCount());
	if (Count() == 0 && size == capacity && hash_map.GetSize() == size * sizeof(ht_entry_t)) {
		return;
	}

	auto new_hash_map = buffer_manager.GetBufferAllocator().AllocateHuge(size * sizeof(ht_entry_t));
	capacity = size;
	hash_map = std::move(new_hash_map);
	entries = reinterpret_cast<ht_entry_t *>(hash_map.get());
	ClearPointerTable();
	bitmask = capacity - 1;

	if (Count() != 0) {
		ReinsertTuples(*partitioned_data);
		if (radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD) {
			ReinsertTuples(*unpartitioned_data);
		}
	}

	Verify();
}

static void SaltIncrementAndWrap(idx_t &offset, const idx_t &salt, const idx_t &capacity_mask) {
	// How many of the uppermost bits of the salt to determine the linear probing increment
	static constexpr idx_t INCREMENT_BITS = 5;
	// Extract the bits and make sure it's odd so we cover the entire domain when adding modulo a power of two
	offset += (salt >> (64 - INCREMENT_BITS)) | 1;
	offset &= capacity_mask;
}

void GroupedAggregateHashTable::ReinsertTuples(PartitionedTupleData &data) {
	for (auto &data_collection : data.GetPartitions()) {
		if (data_collection->Count() == 0) {
			continue;
		}
		TupleDataChunkIterator iterator(*data_collection, TupleDataPinProperties::ALREADY_PINNED, false);
		const auto row_locations = iterator.GetRowLocations();
		do {
			const auto row_count = iterator.GetCurrentChunkCount();
			for (idx_t i = 0; i < row_count; i++) {
				const auto &row_location = row_locations[i];
				const auto hash = Load<hash_t>(row_location + hash_offset);
				const auto salt = ht_entry_t::ExtractSalt(hash);

				// Find an empty entry
				auto ht_offset = ApplyBitMask(hash);
				D_ASSERT(ht_offset == hash % capacity);
				while (entries[ht_offset].IsOccupied()) {
					SaltIncrementAndWrap(ht_offset, salt, bitmask);
				}
				auto &entry = entries[ht_offset];
				D_ASSERT(!entry.IsOccupied());
				entry.SetSalt(salt);
				entry.SetPointer(row_location);
				D_ASSERT(entry.IsOccupied());
			}
		} while (iterator.Next());
	}
}

idx_t GroupedAggregateHashTable::AddChunk(DataChunk &groups, DataChunk &payload, AggregateType filter) {
	unsafe_vector<idx_t> aggregate_filter;

	auto &aggregates = layout_ptr->GetAggregates();
	for (idx_t i = 0; i < aggregates.size(); i++) {
		auto &aggregate = aggregates[i];
		if (aggregate.aggr_type == filter) {
			aggregate_filter.push_back(i);
		}
	}
	return AddChunk(groups, payload, aggregate_filter);
}

idx_t GroupedAggregateHashTable::AddChunk(DataChunk &groups, DataChunk &payload, AggregateType filter,
                                          const before_update_callback_t &before_update) {
	unsafe_vector<idx_t> aggregate_filter;
	auto &aggregates = layout_ptr->GetAggregates();
	for (idx_t i = 0; i < aggregates.size(); i++) {
		if (aggregates[i].aggr_type == filter) {
			aggregate_filter.push_back(i);
		}
	}
	if (groups.size() == 0) {
		return 0;
	}

	adaptivity.AddInput(groups.size());
	groups.Hash(state.hashes);
	const auto new_group_count = FindOrCreateGroups(groups, state.hashes, state.addresses, state.new_groups);
	before_update(state.addresses, state.new_groups, new_group_count);
	VectorOperations::AddInPlace(state.addresses, NumericCast<int64_t>(layout_ptr->GetAggrOffset()));
	UpdateAggregates(payload, aggregate_filter, groups.size());
	return new_group_count;
}

idx_t GroupedAggregateHashTable::AddChunkAndGetNewGroups(DataChunk &groups, DataChunk &payload, AggregateType filter,
                                                         Vector &new_group_addresses, SelectionVector &new_groups_out) {
	unsafe_vector<idx_t> aggregate_filter;
	auto &aggregates = layout_ptr->GetAggregates();
	for (idx_t i = 0; i < aggregates.size(); i++) {
		if (aggregates[i].aggr_type == filter) {
			aggregate_filter.push_back(i);
		}
	}
	if (groups.size() == 0) {
		return 0;
	}

	adaptivity.AddInput(groups.size());
	groups.Hash(state.hashes);
	const auto new_group_count = FindOrCreateGroups(groups, state.hashes, state.addresses, state.new_groups);
	auto source_addresses = FlatVector::GetData<data_ptr_t>(state.addresses);
	auto target_addresses = FlatVector::GetDataMutable<data_ptr_t>(new_group_addresses);
	for (idx_t new_group_idx = 0; new_group_idx < new_group_count; new_group_idx++) {
		const auto input_idx = state.new_groups.get_index_unsafe(new_group_idx);
		new_groups_out.set_index(new_group_idx, input_idx);
		target_addresses[new_group_idx] = source_addresses[input_idx];
	}
	FlatVector::SetSize(new_group_addresses, new_group_count);

	VectorOperations::AddInPlace(state.addresses, NumericCast<int64_t>(layout_ptr->GetAggrOffset()));
	UpdateAggregates(payload, aggregate_filter, groups.size());
	return new_group_count;
}

GroupedAggregateHashTable::AggregateDictionaryState::AggregateDictionaryState()
    : hashes(LogicalType::HASH), new_dictionary_pointers(LogicalType::POINTER), unique_entries(STANDARD_VECTOR_SIZE) {
}

optional_idx GroupedAggregateHashTable::TryAddDictionaryGroups(DataChunk &groups, DataChunk &payload,
                                                               const unsafe_vector<idx_t> &filter) {
	static constexpr idx_t MAX_DICTIONARY_SIZE_THRESHOLD = 20000;
	static constexpr idx_t DICTIONARY_THRESHOLD = 2;
	// dictionary vector - check if this is a duplicate eliminated dictionary from the storage
	const auto &dict_col = groups.data[0];
	auto opt_dict_size = DictionaryVector::DictionarySize(dict_col);
	if (!opt_dict_size.IsValid()) {
		// dict size not known - this is not a dictionary that comes from the storage
		return optional_idx();
	}
	idx_t dict_size = opt_dict_size.GetIndex();
	auto &dictionary_id = DictionaryVector::DictionaryId(dict_col);
	if (dictionary_id.empty()) {
		// dictionary has no id, we can't cache across vectors
		// only use dictionary compression if there are fewer entries than groups
		if (dict_size * DICTIONARY_THRESHOLD >= groups.size()) {
			// dictionary is too large - use regular aggregation
			return optional_idx();
		}
	} else {
		// dictionary has an id - we can cache across vectors
		// use a much larger limit for dictionary
		if (dict_size >= MAX_DICTIONARY_SIZE_THRESHOLD) {
			// dictionary is too large - use regular aggregation
			return optional_idx();
		}
	}
	const auto &dictionary_vector = DictionaryVector::Child(dict_col);
	const auto &offsets = DictionaryVector::SelVector(dict_col);
	auto &dict_state = state.dict_state;
	if (dict_state.dictionary_id.empty() || dict_state.dictionary_id != dictionary_id) {
		// new dictionary - initialize the index state
		if (dict_size > dict_state.capacity) {
			dict_state.dictionary_addresses = make_uniq<Vector>(LogicalType::POINTER, dict_size);
			dict_state.found_entry = make_unsafe_uniq_array<bool>(dict_size);
			dict_state.capacity = dict_size;
		}
		memset(dict_state.found_entry.get(), 0, dict_size * sizeof(bool));
		dict_state.dictionary_id = dictionary_id;
		dict_state.resolved_count = 0;
		dict_state.address_high_bits = ~uint64_t(0);
		dict_state.address_high_bits_uniform =
		    (sizeof(uintptr_t) == sizeof(uint64_t)); // only relevant on 64-bits systems
	} else if (dict_size > dict_state.capacity) {
		throw InternalException("AggregateHT - using cached dictionary data but dictionary has changed (dictionary id "
		                        "%s - dict size %d, current capacity %d)",
		                        dict_state.dictionary_id, dict_size, dict_state.capacity);
	}

	auto &found_entry = dict_state.found_entry;
	auto &unique_entries = dict_state.unique_entries;
	idx_t unique_count = 0;
	// for each of the dictionary entries - check if we have already done a look-up into the hash table
	// if we have, we can just use the cached group pointers
	// once every dict slot has been seen the walk would produce no new entries - skip it
	if (dict_state.resolved_count < dict_size) {
		for (idx_t i = 0; i < groups.size(); i++) {
			auto dict_idx = offsets.get_index(i);
			unique_entries.set_index(unique_count, dict_idx);
			unique_count += !found_entry[dict_idx];
			found_entry[dict_idx] = true;
		}
		dict_state.resolved_count += unique_count;
	}
	auto &new_dictionary_pointers = dict_state.new_dictionary_pointers;
	idx_t new_group_count = 0;
	if (unique_count > 0) {
		auto &unique_values = dict_state.unique_values;
		if (unique_values.ColumnCount() == 0) {
			unique_values.InitializeEmpty(groups.GetTypes());
		}
		// slice the dictionary
		unique_values.data[0].Slice(dictionary_vector, unique_entries, unique_count);
		unique_values.CheckCardinality(unique_count);
		// now we know which entries we are going to add - hash them
		auto &hashes = dict_state.hashes;
		unique_values.Hash(hashes);

		// add the dictionary groups to the hash table
		new_group_count = FindOrCreateGroups(unique_values, hashes, new_dictionary_pointers, state.new_groups);
	}
	auto &aggregates = layout_ptr->GetAggregates();
	if (aggregates.empty()) {
		// early-out - no aggregates to update
		return new_group_count;
	}

	// set the addresses that we found for each of the unique groups in the main addresses vector
	auto new_dict_addresses = FlatVector::GetData<uintptr_t>(new_dictionary_pointers);
	// for each of the new groups, add them to the global (cached) list of addresses for the dictionary
	auto &dictionary_addresses = *dict_state.dictionary_addresses;
	auto dict_addresses = FlatVector::ScatterWriter<uintptr_t>(dictionary_addresses);
	for (idx_t i = 0; i < unique_count; i++) {
		auto dict_idx = unique_entries.get_index(i);
		const uintptr_t addr = new_dict_addresses[i] + layout_ptr->GetAggrOffset();
		static constexpr uint64_t GID_HIGH_MASK = ~(ClusteredAggr::MAX_GID_COUNT - 1);
		if (dict_state.address_high_bits_uniform) { // for clustered aggregation: check high bit uniformity
			const uint64_t high_bits = static_cast<uint64_t>(addr) & GID_HIGH_MASK;
			if (dict_state.address_high_bits == ~uint64_t(0)) { // uninitialized
				dict_state.address_high_bits = high_bits;
			} else if (high_bits != dict_state.address_high_bits) {
				dict_state.address_high_bits_uniform = false;
			}
		}
		dict_addresses[dict_idx] = addr;
	}
	// now set up the addresses for the aggregates
	auto result_addresses = FlatVector::Writer<uintptr_t>(state.addresses, groups.size());
	for (idx_t i = 0; i < groups.size(); i++) {
		auto dict_idx = offsets.get_index(i);
		result_addresses.WriteValue(dict_addresses[dict_idx]);
	}
	FlatVector::SetSize(state.addresses, groups.size());

	// finally process the aggregates (ht_offsets are only valid for unique entries, not the full payload)
	UpdateAggregates(payload, filter, groups.size(), false);

	return new_group_count;
}

optional_idx GroupedAggregateHashTable::TryAddConstantGroups(DataChunk &groups, DataChunk &payload,
                                                             const unsafe_vector<idx_t> &filter) {
#ifndef DEBUG
	if (groups.size() <= 1) {
		// this only has a point if we have multiple groups
		return optional_idx();
	}
#endif
	// Capture row_count before Reference+SetChildCardinality, which share the buffer and would corrupt groups.size()
	const idx_t row_count = groups.size();
	auto &dict_state = state.dict_state;
	auto &unique_values = dict_state.unique_values;
	if (unique_values.ColumnCount() == 0) {
		unique_values.InitializeEmpty(groups.GetTypes());
	}
	// slice the dictionary
	unique_values.Reference(groups);
	unique_values.SetChildCardinality(1);
	unique_values.Flatten();
	// Restore the groups chunk's buffer v_size which was corrupted to 1 by SetChildCardinality(1) above.
	// unique_values.Flatten() created new independent buffers, so restoring groups is safe.
	groups.SetChildCardinality(row_count);

	auto &hashes = dict_state.hashes;
	unique_values.Hash(hashes);

	// add the single constant group to the hash table
	auto &new_dictionary_pointers = dict_state.new_dictionary_pointers;
	auto new_group_count = FindOrCreateGroups(unique_values, hashes, new_dictionary_pointers, state.new_groups);

	auto &aggregates = layout_ptr->GetAggregates();
	if (aggregates.empty()) {
		// early-out - no aggregates to update
		return new_group_count;
	}

	// process the aggregates
	// FIXME: This should just be a CONSTANT_VECTOR but subsequent operations assume FLAT_VECTOR
	auto new_dict_addresses = FlatVector::GetData<uintptr_t>(new_dictionary_pointers);
	auto result_addresses = FlatVector::Writer<uintptr_t>(state.addresses, row_count);
	uintptr_t aggregate_address = new_dict_addresses[0] + layout_ptr->GetAggrOffset();
	static constexpr uint64_t GID_HIGH_MASK = ~(ClusteredAggr::MAX_GID_COUNT - 1);
	dict_state.address_high_bits_uniform = (sizeof(uintptr_t) == sizeof(uint64_t));
	dict_state.address_high_bits = static_cast<uint64_t>(aggregate_address) & GID_HIGH_MASK;
	for (idx_t i = 0; i < row_count; i++) {
		result_addresses.WriteValue(aggregate_address);
	}
	FlatVector::SetSize(state.addresses, row_count);
	state.addresses.SetVectorType(VectorType::CONSTANT_VECTOR);
	// ht_offsets are only valid for the single constant group, not the full payload
	UpdateAggregates(payload, filter, row_count, false);
	state.addresses.SetVectorType(VectorType::FLAT_VECTOR);

	return new_group_count;
}

optional_idx GroupedAggregateHashTable::TryAddCompressedGroups(DataChunk &groups, DataChunk &payload,
                                                               const unsafe_vector<idx_t> &filter) {
	// all groups must be compressed
	if (groups.AllConstant()) {
		return TryAddConstantGroups(groups, payload, filter);
	}
	if (groups.ColumnCount() == 1 && groups.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR &&
	    groups.data[0].GetType().InternalType() != PhysicalType::STRUCT) {
		return TryAddDictionaryGroups(groups, payload, filter);
	}
	return optional_idx();
}

idx_t GroupedAggregateHashTable::AddChunk(DataChunk &groups, DataChunk &payload, const unsafe_vector<idx_t> &filter) {
	adaptivity.AddInput(groups.size());

	// check if we can use an optimized path that utilizes compressed vectors
	auto result = TryAddCompressedGroups(groups, payload, filter);
	if (result.IsValid()) {
		return result.GetIndex();
	}
	// otherwise append the raw values
	groups.Hash(state.hashes);

	return AddChunk(groups, state.hashes, payload, filter);
}

bool GroupedAggregateHashTable::UpdateAggregatesClustered(DataChunk &payload, const unsafe_vector<idx_t> &filter,
                                                          idx_t count, bool ht_offsets_valid) {
	if (adaptivity.LookupsSkipped()) {
		return false;
	}
	if (sizeof(uintptr_t) < sizeof(uint64_t)) {
		return false;
	}
	ClusteredAggr clustered;
	if (ht_offsets_valid) {
		if (capacity >= ClusteredAggr::MAX_GID_COUNT) {
			return false;
		}
		if (!clustered_state.TryBuild(clustered, FlatVector::GetData<uint64_t>(state.ht_offsets), count)) {
			return false;
		}
		const auto aggr_offset = layout_ptr->GetAggrOffset();
		clustered.InitializeStates([&](uint64_t gid) {
			auto slot = static_cast<idx_t>(gid);
			return entries[slot].GetPointer() + aggr_offset;
		});
	} else {
		// dictionary path: addresses are already resolved pointers — and gids are not even set then
		// But, we can use the "addresses as gids". ClusteredAggr only triggers on low gid counts (see above) and
		// for speed the TryClustered method exploits that by ignoring high gid bits. When using "addresses as gids"
		// we thus need to know that all high bits are the same (this is very typically the case).
		if (!state.dict_state.address_high_bits_uniform) {
			return false;
		}
		auto addrs = FlatVector::GetData<uint64_t>(state.addresses);
		if (!clustered_state.TryBuild(clustered, addrs, count)) {
			return false;
		}
		clustered.InitializeStates(
		    [&](uint64_t gid) { return reinterpret_cast<data_ptr_t>(state.dict_state.address_high_bits | gid); });
	}

	const bool skip_addresses = clustered_state.all_clustered;
	auto &aggregates = layout_ptr->GetAggregates();
	RowOperations::UpdateStatesClustered(state.row_state, aggregates, &filter_set, &filter, state.addresses, payload,
	                                     clustered, skip_addresses);
	return true;
}

void GroupedAggregateHashTable::UpdateAggregates(DataChunk &payload, const unsafe_vector<idx_t> &filter, idx_t count,
                                                 bool ht_offsets_valid) {
	if (UpdateAggregatesClustered(payload, filter, count, ht_offsets_valid)) {
		Verify();
		return;
	}

	auto &aggregates = layout_ptr->GetAggregates();
	idx_t filter_idx = 0;
	idx_t payload_idx = 0;
	for (idx_t i = 0; i < aggregates.size(); i++) {
		auto &aggr = aggregates[i];
		if (filter_idx >= filter.size() || i < filter[filter_idx]) {
			// Skip all the aggregates that are not in the filter
			payload_idx += aggr.child_count;
			VectorOperations::AddInPlace(state.addresses, NumericCast<int64_t>(aggr.payload_size));
			continue;
		}
		D_ASSERT(i == filter[filter_idx]);

		if (aggr.aggr_type != AggregateType::DISTINCT && aggr.filter) {
			RowOperations::UpdateFilteredStates(state.row_state, filter_set.GetFilterData(i), aggr, state.addresses,
			                                    payload, payload_idx);
		} else {
			RowOperations::UpdateStates(state.row_state, aggr, state.addresses, payload, payload_idx);
		}

		// Move to the next aggregate
		payload_idx += aggr.child_count;
		VectorOperations::AddInPlace(state.addresses, NumericCast<int64_t>(aggr.payload_size));
		filter_idx++;
	}

	Verify();
}

idx_t GroupedAggregateHashTable::AddChunk(DataChunk &groups, Vector &group_hashes, DataChunk &payload,
                                          const unsafe_vector<idx_t> &filter) {
	if (groups.size() == 0) {
		return 0;
	}

#ifdef DEBUG
	D_ASSERT(groups.ColumnCount() + 1 == layout_ptr->ColumnCount());
	for (idx_t i = 0; i < groups.ColumnCount(); i++) {
		D_ASSERT(groups.GetTypes()[i] == layout_ptr->GetTypes()[i]);
	}
#endif

	const auto new_group_count = FindOrCreateGroups(groups, group_hashes, state.addresses, state.new_groups);
	VectorOperations::AddInPlace(state.addresses, NumericCast<int64_t>(layout_ptr->GetAggrOffset()));

	UpdateAggregates(payload, filter, groups.size());

	return new_group_count;
}

void GroupedAggregateHashTable::FetchAggregates(DataChunk &groups, DataChunk &result) {
#ifdef DEBUG
	groups.Verify(context.db);
	D_ASSERT(groups.ColumnCount() + 1 == layout_ptr->ColumnCount());
	for (idx_t i = 0; i < result.ColumnCount(); i++) {
		D_ASSERT(result.data[i].GetType() == payload_types[i]);
	}
#endif

	result.SetChildCardinality(groups.size());
	if (groups.size() == 0) {
		return;
	}

	// find the groups associated with the addresses
	// FIXME: this should not use the FindOrCreateGroups, creating them is unnecessary
	FindOrCreateGroups(groups, state.addresses);
	// now fetch the aggregates
	RowOperations::FinalizeStates(state.row_state, *layout_ptr, state.addresses, result, 0);
}

template <bool HAS_SEL, bool CREATE_MISSING>
static void GroupedAggregateHashTableInnerLoop(ht_entry_t *const entries, const idx_t capacity, const hash_t bitmask,
                                               const hash_t *const hash_salts, uint64_t *const ht_offsets,
                                               const SelectionVector *const sel_vector, const idx_t remaining_entries,
                                               SelectionVector &empty_vector, SelectionVector &compare_vector,
                                               idx_t &empty_count, idx_t &compare_count) {
	// For each remaining entry, figure out whether or not it belongs to a full or empty group
	for (idx_t i = 0; i < remaining_entries; i++) {
		PrefetchHashTableEntry(i, remaining_entries, [&](idx_t next_idx) {
			const auto next_index = HAS_SEL ? sel_vector->get_index_unsafe(next_idx) : next_idx;
			return &entries[ht_offsets[next_index]];
		});
		const auto index = HAS_SEL ? sel_vector->get_index_unsafe(i) : i;
		const auto salt = hash_salts[index];
		auto &ht_offset = ht_offsets[index];

		idx_t inner_iteration_count;
		for (inner_iteration_count = 0; inner_iteration_count < capacity; inner_iteration_count++) {
			auto &entry = entries[ht_offset];
			if (!entry.IsOccupied()) {
				if (CREATE_MISSING) {
					entry.SetSalt(salt);
				}
				empty_vector.set_index(empty_count++, index);
				break;
			}

			if (DUCKDB_LIKELY(entry.GetSalt() == salt)) { // Matching salt: compare groups
				compare_vector.set_index(compare_count++, index);
				break;
			}

			// Linear probing
			SaltIncrementAndWrap(ht_offset, salt, bitmask);
		}
		if (DUCKDB_UNLIKELY(inner_iteration_count == capacity)) {
			throw InternalException("Maximum inner iteration count reached in GroupedAggregateHashTable");
		}
	}
}

#if defined(__GNUC__)
__attribute__((aligned(128)))
#endif
idx_t GroupedAggregateHashTable::FindOrCreateGroupsInternal(DataChunk &groups, Vector &group_hashes_v,
                                                            Vector &addresses_v, SelectionVector &new_groups_out,
                                                            optional_ptr<TupleDataChunkState> source_rows) {
	D_ASSERT(state.initialized && state.owner.get() == this);
	D_ASSERT(groups.ColumnCount() + 1 == layout_ptr->ColumnCount());
	D_ASSERT(group_hashes_v.GetType() == LogicalType::HASH);
	D_ASSERT(state.ht_offsets.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(state.ht_offsets.GetType() == LogicalType::UBIGINT);
	D_ASSERT(addresses_v.GetType() == LogicalType::POINTER);
	D_ASSERT(state.hash_salts.GetType() == LogicalType::HASH);

	// Need to fit the entire vector, and resize at threshold
	const auto chunk_size = groups.size();
	if (Count() + chunk_size > capacity || Count() + chunk_size > ResizeThreshold()) {
		Verify();
		Resize(capacity * 2);
	}
	D_ASSERT(capacity - Count() >= chunk_size); // we need to be able to fit at least one vector of data

	// we start out with all entries [0, 1, 2, ..., chunk_size]
	const SelectionVector *sel_vector = FlatVector::IncrementalSelectionVector();

	// Make a chunk that references the groups and the hashes and convert to unified format
	if (state.group_chunk.ColumnCount() == 0) {
		state.group_chunk.InitializeEmpty(layout_ptr->GetTypes());
	}
	D_ASSERT(state.group_chunk.ColumnCount() == layout_ptr->GetTypes().size());
	for (idx_t grp_idx = 0; grp_idx < groups.ColumnCount(); grp_idx++) {
		state.group_chunk.data[grp_idx].Reference(groups.data[grp_idx]);
	}
	state.group_chunk.data[groups.ColumnCount()].Reference(group_hashes_v);

	// convert all vectors to unified format
	TupleDataCollection::ToUnifiedFormat(state.partitioned_append_state.chunk_state, state.group_chunk);

	const auto hashes = group_hashes_v.Values<hash_t>();

	addresses_v.Flatten();
	const auto addresses = FlatVector::GetDataMutable<data_ptr_t>(addresses_v);

	if (adaptivity.LookupsSkipped()) {
		if (adaptivity.HLLEnabled()) {
			adaptivity.ObserveHashes(group_hashes_v);
		}
		// Just appending now
		if (source_rows) {
			partitioned_data->Append(state.partitioned_append_state, *source_rows, chunk_size);
		} else {
			partitioned_data->AppendUnified(state.partitioned_append_state, state.group_chunk,
			                                *FlatVector::IncrementalSelectionVector(), chunk_size);
			RowOperations::InitializeStates(*layout_ptr, state.partitioned_append_state.chunk_state.row_locations,
			                                *FlatVector::IncrementalSelectionVector(), chunk_size);
		}

		const auto row_locations =
		    FlatVector::GetData<data_ptr_t>(state.partitioned_append_state.chunk_state.row_locations);
		const auto &row_sel = state.partitioned_append_state.reverse_partition_sel;
		for (idx_t i = 0; i < chunk_size; i++) {
			const auto row_idx = row_sel.get_index_unsafe(i);
			const auto &row_location = row_locations[row_idx];
			addresses[i] = row_location;
		}
		count += chunk_size;
		FlatVector::SetSize(addresses_v, chunk_size);
		return chunk_size;
	}

	// Compute the entry in the table based on the hash using a modulo,
	// and precompute the hash salts for faster comparison below
	const auto ht_offsets = FlatVector::GetDataMutable<uint64_t>(state.ht_offsets);
	const auto hash_salts = FlatVector::GetDataMutable<hash_t>(state.hash_salts);

	for (idx_t r = 0; r < chunk_size; r++) {
		const auto &hash = hashes[r].GetValue();
		auto &ht_offset = ht_offsets[r];
		ht_offset = ApplyBitMask(hash);
		D_ASSERT(ht_offset == hash % capacity);
		hash_salts[r] = ht_entry_t::ExtractSalt(hash);
	}

	SelectionVector empty_vector;
	idx_t new_group_count = 0;
	idx_t remaining_entries = chunk_size;
	idx_t iteration_count;
	for (iteration_count = 0; remaining_entries > 0 && iteration_count < capacity; iteration_count++) {
		idx_t new_entry_count = 0;
		idx_t need_compare_count = 0;
		idx_t no_match_count = 0;

		// "new_groups_out" contains ALL groups for the chunk, "empty_vector" only the groups for this iteration,
		// so it's just the same selection vector, but offset by the current "new_group_count".
		empty_vector.Initialize(new_groups_out.data() + new_group_count, new_groups_out.Capacity() - new_group_count);
		if (sel_vector->IsSet()) {
			GroupedAggregateHashTableInnerLoop<true, true>(
			    entries, capacity, bitmask, hash_salts, ht_offsets, sel_vector, remaining_entries, empty_vector,
			    state.group_compare_vector, new_entry_count, need_compare_count);
		} else {
			GroupedAggregateHashTableInnerLoop<false, true>(
			    entries, capacity, bitmask, hash_salts, ht_offsets, sel_vector, remaining_entries, empty_vector,
			    state.group_compare_vector, new_entry_count, need_compare_count);
		}
		new_group_count += new_entry_count;

		if (new_entry_count != 0) {
			// Append everything that belongs to an empty group
			optional_ptr<PartitionedTupleData> data;
			optional_ptr<PartitionedTupleDataAppendState> append_state;
			if (radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD &&
			    new_entry_count / RadixPartitioning::NumberOfPartitions(radix_bits) <= 4) {
				TupleDataCollection::ToUnifiedFormat(state.unpartitioned_append_state.chunk_state, state.group_chunk);
				data = unpartitioned_data.get();
				append_state = &state.unpartitioned_append_state;
			} else {
				data = partitioned_data.get();
				append_state = &state.partitioned_append_state;
			}
			if (source_rows) {
				data->Append(*append_state, *source_rows, new_entry_count, empty_vector);
			} else {
				data->AppendUnified(*append_state, state.group_chunk, empty_vector, new_entry_count);
				RowOperations::InitializeStates(*layout_ptr, append_state->chunk_state.row_locations,
				                                *FlatVector::IncrementalSelectionVector(), new_entry_count);
			}

			// Set the entry pointers in the 1st part of the HT now that the data has been appended
			const auto row_locations = FlatVector::GetData<data_ptr_t>(append_state->chunk_state.row_locations);
			const auto &row_sel = append_state->reverse_partition_sel;
			for (idx_t new_entry_idx = 0; new_entry_idx < new_entry_count; new_entry_idx++) {
				const auto index = empty_vector.get_index_unsafe(new_entry_idx);
				const auto row_idx = row_sel.get_index_unsafe(index);
				const auto &row_location = row_locations[row_idx];

				auto &entry = entries[ht_offsets[index]];

				entry.SetPointer(row_location);
				addresses[index] = row_location;
			}
		}

		if (need_compare_count != 0) {
			// Get the pointers to the rows that need to be compared
			for (idx_t need_compare_idx = 0; need_compare_idx < need_compare_count; need_compare_idx++) {
				const auto index = state.group_compare_vector.get_index_unsafe(need_compare_idx);
				const auto &entry = entries[ht_offsets[index]];
				addresses[index] = entry.GetPointer();
			}

			// Perform group comparisons
			row_matcher.Match(state.group_chunk, state.partitioned_append_state.chunk_state.vector_data,
			                  state.group_compare_vector, need_compare_count, addresses_v, &state.no_match_vector,
			                  no_match_count);
		}

		// Linear probing: each of the entries that do not match move to the next entry in the HT
		for (idx_t i = 0; i < no_match_count; i++) {
			const auto index = state.no_match_vector.get_index_unsafe(i);
			auto &ht_offset = ht_offsets[index];
			const auto salt = hash_salts[index];
			SaltIncrementAndWrap(ht_offset, salt, bitmask);
		}
		sel_vector = &state.no_match_vector;
		remaining_entries = no_match_count;
	}
	if (iteration_count == capacity) {
		throw InternalException("Maximum outer iteration count reached in GroupedAggregateHashTable");
	}

	if (adaptivity.HLLEnabled() && new_group_count > 0) {
		if (new_group_count == chunk_size) {
			adaptivity.ObserveHashes(group_hashes_v);
		} else {
			adaptivity.ObserveHashes(hashes, new_groups_out, new_group_count);
		}
	}

	FlatVector::SetSize(addresses_v, chunk_size);
	count += new_group_count;
	return new_group_count;
}

// this is to support distinct aggregations where we need to record whether we
// have already seen a value for a group
idx_t GroupedAggregateHashTable::FindOrCreateGroups(DataChunk &groups, Vector &group_hashes, Vector &addresses_out,
                                                    SelectionVector &new_groups_out) {
	return FindOrCreateGroupsInternal(groups, group_hashes, addresses_out, new_groups_out);
}

void GroupedAggregateHashTable::FindOrCreateGroups(DataChunk &groups, Vector &addresses) {
	// create a dummy new_groups sel vector
	FindOrCreateGroups(groups, addresses, state.new_groups);
}

idx_t GroupedAggregateHashTable::FindOrCreateGroups(DataChunk &groups, Vector &addresses_out,
                                                    SelectionVector &new_groups_out) {
	groups.Hash(state.hashes);
	return FindOrCreateGroups(groups, state.hashes, addresses_out, new_groups_out);
}

void GroupedAggregateHashTable::InitializeLookupState(AggregateHTLookupState &lookup_state) const {
	if (lookup_state.initialized) {
		if (lookup_state.owner.get() != this) {
			throw InternalException("Aggregate hash-table lookup state cannot be reused across hash tables");
		}
		return;
	}
	lookup_state.group_chunk.InitializeEmpty(layout_ptr->GetTypes());
	TupleDataCollection::InitializeChunkState(lookup_state.chunk_state, layout_ptr->GetTypes());
	lookup_state.row_matcher.Initialize(true, *layout_ptr, predicates);
	for (idx_t group_idx = 0; group_idx + 1 < layout_ptr->ColumnCount(); group_idx++) {
		lookup_state.gather_functions.push_back(
		    TupleDataCollection::GetGatherFunction(layout_ptr->GetTypes()[group_idx]));
	}
	lookup_state.owner = this;
	lookup_state.initialized = true;
}

idx_t GroupedAggregateHashTable::LookupGroups(DataChunk &groups, AggregateHTLookupState &lookup_state,
                                              SelectionVector &found_groups_out) const {
	D_ASSERT(groups.ColumnCount() + 1 == layout_ptr->ColumnCount());
	const auto chunk_size = groups.size();
	if (chunk_size == 0) {
		return 0;
	}
	InitializeLookupState(lookup_state);

	groups.Hash(lookup_state.hashes);
	for (idx_t group_idx = 0; group_idx < groups.ColumnCount(); group_idx++) {
		lookup_state.group_chunk.data[group_idx].Reference(groups.data[group_idx]);
	}
	lookup_state.group_chunk.data[groups.ColumnCount()].Reference(lookup_state.hashes);
	lookup_state.group_chunk.CheckCardinality(chunk_size);
	TupleDataCollection::ToUnifiedFormat(lookup_state.chunk_state, lookup_state.group_chunk);

	const auto hashes = lookup_state.hashes.Values<hash_t>();
	const auto ht_offsets = FlatVector::GetDataMutable<uint64_t>(lookup_state.ht_offsets);
	const auto hash_salts = FlatVector::GetDataMutable<hash_t>(lookup_state.hash_salts);
	FlatVector::SetSize(lookup_state.addresses, chunk_size);
	lookup_state.addresses.Flatten();
	const auto addresses = FlatVector::GetDataMutable<data_ptr_t>(lookup_state.addresses);

	for (idx_t row_idx = 0; row_idx < chunk_size; row_idx++) {
		const auto hash = hashes[row_idx].GetValue();
		ht_offsets[row_idx] = ApplyBitMask(hash);
		hash_salts[row_idx] = ht_entry_t::ExtractSalt(hash);
	}

	const SelectionVector *remaining_sel = FlatVector::IncrementalSelectionVector();
	idx_t remaining_count = chunk_size;
	idx_t found_count = 0;
	idx_t iteration_count;
	for (iteration_count = 0; remaining_count > 0 && iteration_count < capacity; iteration_count++) {
		idx_t missing_count = 0;
		idx_t compare_count = 0;
		if (remaining_sel->IsSet()) {
			GroupedAggregateHashTableInnerLoop<true, false>(
			    entries, capacity, bitmask, hash_salts, ht_offsets, remaining_sel, remaining_count,
			    lookup_state.missing_vector, lookup_state.group_compare_vector, missing_count, compare_count);
		} else {
			GroupedAggregateHashTableInnerLoop<false, false>(
			    entries, capacity, bitmask, hash_salts, ht_offsets, remaining_sel, remaining_count,
			    lookup_state.missing_vector, lookup_state.group_compare_vector, missing_count, compare_count);
		}

		for (idx_t compare_idx = 0; compare_idx < compare_count; compare_idx++) {
			const auto index = lookup_state.group_compare_vector.get_index_unsafe(compare_idx);
			addresses[index] = entries[ht_offsets[index]].GetPointer();
		}

		idx_t no_match_count = 0;
		if (compare_count > 0) {
			const auto match_count = lookup_state.row_matcher.Match(
			    lookup_state.group_chunk, lookup_state.chunk_state.vector_data, lookup_state.group_compare_vector,
			    compare_count, lookup_state.addresses, &lookup_state.no_match_vector, no_match_count);
			for (idx_t match_idx = 0; match_idx < match_count; match_idx++) {
				found_groups_out.set_index(found_count++,
				                           lookup_state.group_compare_vector.get_index_unsafe(match_idx));
			}
		}

		for (idx_t no_match_idx = 0; no_match_idx < no_match_count; no_match_idx++) {
			const auto index = lookup_state.no_match_vector.get_index_unsafe(no_match_idx);
			SaltIncrementAndWrap(ht_offsets[index], hash_salts[index], bitmask);
		}
		remaining_sel = &lookup_state.no_match_vector;
		remaining_count = no_match_count;
	}
	if (iteration_count == capacity && remaining_count > 0) {
		throw InternalException("Maximum outer iteration count reached in GroupedAggregateHashTable lookup");
	}
	std::sort(found_groups_out.data(), found_groups_out.data() + found_count);
	return found_count;
}

void GroupedAggregateHashTable::GatherGroups(AggregateHTLookupState &state, const SelectionVector &found_groups,
                                             idx_t found_count, DataChunk &result) const {
	D_ASSERT(result.ColumnCount() == layout_ptr->ColumnCount() - 1);
	D_ASSERT(state.gather_functions.size() == result.ColumnCount());
	result.Reset();
	for (idx_t group_idx = 0; group_idx < result.ColumnCount(); group_idx++) {
		state.gather_functions[group_idx].Gather(*layout_ptr, state.addresses, group_idx, found_groups, found_count,
		                                         result.data[group_idx], *FlatVector::IncrementalSelectionVector(),
		                                         nullptr);
	}
	result.SetChildCardinality(found_count);
}

void GroupedAggregateHashTable::GatherGroups(AggregateHTLookupState &state, Vector &addresses,
                                             const SelectionVector &groups, idx_t group_count,
                                             DataChunk &result) const {
	InitializeLookupState(state);
	D_ASSERT(result.ColumnCount() == layout_ptr->ColumnCount() - 1);
	result.Reset();
	for (idx_t group_idx = 0; group_idx < result.ColumnCount(); group_idx++) {
		state.gather_functions[group_idx].Gather(*layout_ptr, addresses, group_idx, groups, group_count,
		                                         result.data[group_idx], *FlatVector::IncrementalSelectionVector(),
		                                         nullptr);
	}
	result.SetChildCardinality(group_count);
}

struct FlushMoveState {
	explicit FlushMoveState(TupleDataCollection &collection_p)
	    : collection(collection_p), hashes(LogicalType::HASH), group_addresses(LogicalType::POINTER),
	      new_groups_sel(STANDARD_VECTOR_SIZE) {
		const auto &layout = collection.GetLayout();
		vector<column_t> column_ids;
		column_ids.reserve(layout.ColumnCount() - 1);
		for (idx_t col_idx = 0; col_idx < layout.ColumnCount() - 1; col_idx++) {
			column_ids.emplace_back(col_idx);
		}
		collection.InitializeScan(scan_state, column_ids, TupleDataPinProperties::DESTROY_AFTER_DONE);
		collection.InitializeScanChunk(scan_state, groups);
		hash_col_idx = layout.ColumnCount() - 1;
	}

	bool Scan() {
		if (collection.Scan(scan_state, groups)) {
			collection.Gather(scan_state.chunk_state.row_locations, *FlatVector::IncrementalSelectionVector(),
			                  groups.size(), hash_col_idx, hashes, *FlatVector::IncrementalSelectionVector(), nullptr);
			return true;
		}

		collection.FinalizePinState(scan_state.pin_state);
		return false;
	}

	TupleDataCollection &collection;
	TupleDataScanState scan_state;
	DataChunk groups;

	idx_t hash_col_idx;
	Vector hashes;

	Vector group_addresses;
	SelectionVector new_groups_sel;
};

void GroupedAggregateHashTable::Combine(GroupedAggregateHashTable &other) {
	auto other_partitioned_data = other.AcquirePartitionedData();
	auto other_data = other_partitioned_data->GetUnpartitioned();
	Combine(*other_data);

	// Inherit ownership to all stored aggregate allocators
	stored_allocators.emplace_back(other.aggregate_allocator);
	for (const auto &stored_allocator : other.stored_allocators) {
		stored_allocators.emplace_back(stored_allocator);
	}
}

void GroupedAggregateHashTable::Combine(TupleDataCollection &other_data, optional_ptr<atomic<idx_t>> combined_chunks) {
	D_ASSERT(other_data.GetLayout().GetAggrWidth() == layout_ptr->GetAggrWidth());
	D_ASSERT(other_data.GetLayout().GetDataWidth() == layout_ptr->GetDataWidth());
	D_ASSERT(other_data.GetLayout().GetRowWidth() == layout_ptr->GetRowWidth());

	if (other_data.Count() == 0) {
		return;
	}

	FlushMoveState fm_state(other_data);

	while (fm_state.Scan()) {
		// Check for interrupts with each chunk
		context.InterruptCheck();
		auto &source_rows = fm_state.scan_state.chunk_state;
		const auto chunk_count = fm_state.groups.size();
		if (!layout_ptr->AllConstant()) {
			other_data.FindHeapPointers(source_rows, chunk_count);
		}
		const auto new_count = FindOrCreateGroupsInternal(fm_state.groups, fm_state.hashes, fm_state.group_addresses,
		                                                  fm_state.new_groups_sel, source_rows);
		if (new_count != chunk_count) {
			auto &sources = source_rows.row_locations;
			auto &targets = fm_state.group_addresses;
			if (new_count != 0) {
				// Newly copied states need no combination or destruction.
				auto target_pointers = FlatVector::ScatterWriter<data_ptr_t>(targets);
				for (idx_t i = 0; i < new_count; i++) {
					target_pointers[fm_state.new_groups_sel.get_index_unsafe(i)] = nullptr;
				}

				const auto combine_count = chunk_count - new_count;
				const auto source_values = sources.Values<data_ptr_t>();
				const auto target_values = targets.Values<data_ptr_t>();
				auto source_writer = FlatVector::Writer<data_ptr_t>(sources, combine_count);
				auto target_writer = FlatVector::Writer<data_ptr_t>(targets, combine_count);
				// Compact forwards so unread pointers are never overwritten.
				for (idx_t i = 0; i < chunk_count; i++) {
					const auto target = target_values[i].GetValue();
					if (target) {
						source_writer.WriteValue(source_values[i].GetValue());
						target_writer.WriteValue(target);
					}
				}
			}
			RowOperations::CombineStates(state.row_state, *layout_ptr, sources, targets);
			if (layout_ptr->HasDestructor()) {
				RowOperations::DestroyStates(state.row_state, *layout_ptr, sources);
			}
		}

		if (combined_chunks) {
			combined_chunks->fetch_add(1, std::memory_order_relaxed);
		}
	}

	Verify();
}

void GroupedAggregateHashTable::InitializeScan(AggregateHTScanState &scan_state) {
	scan_state.partition_idx = 0;
	vector<idx_t> group_indexes(layout_ptr->ColumnCount() - 1);
	for (idx_t i = 0; i < group_indexes.size(); i++) {
		group_indexes[i] = i;
	}

	auto &partition = partitioned_data->GetPartitions()[scan_state.partition_idx];
	partition->InitializeScan(scan_state.scan_states, group_indexes);
}

bool GroupedAggregateHashTable::ScanGroups(AggregateHTScanState &scan_state, DataChunk &distinct_rows) {
	if (scan_state.partition_idx >= partitioned_data->PartitionCount()) {
		return false;
	}

	distinct_rows.Reset();
	auto &current_partition = partitioned_data->GetPartitions()[scan_state.partition_idx];

	if (current_partition->Scan(scan_state.scan_states, distinct_rows)) {
		return true;
	} else {
		if (++(scan_state.partition_idx) >= partitioned_data->PartitionCount()) {
			return false;
		} else {
			auto &new_partition = partitioned_data->GetPartitions()[scan_state.partition_idx];
			new_partition->InitializeScan(scan_state.scan_states);
			return true;
		}
	}
}

bool GroupedAggregateHashTable::Scan(AggregateHTScanState &scan_state, DataChunk &distinct_rows,
                                     DataChunk &payload_rows) {
	payload_rows.Reset();
	if (!ScanGroups(scan_state, distinct_rows)) {
		return false;
	}
	if (distinct_rows.size() > 0) {
		FetchAggregates(distinct_rows, payload_rows);
	}
	return true;
}

void GroupedAggregateHashTable::ResetForNewIteration(idx_t radix_bits_p) {
	tuple_size_cache.count.SetInvalid();
	// Save the previous iteration's group count before destroying aggregate states.
	// This lets us size the pointer table based on actual prior data rather than the
	// global sink capacity, which is typically much larger than recursive iteration sizes.
	const auto prev_count = count;
	Destroy();
	aggregate_allocator->Reset();
	stored_allocators.clear();

	const auto reuse_partitioned_data = partitioned_data && RadixPartitioning::RadixBitsOfPowerOfTwo(
	                                                            partitioned_data->PartitionCount()) == radix_bits_p;
	const auto reuse_unpartitioned_data =
	    radix_bits_p >= UNPARTITIONED_RADIX_BITS_THRESHOLD && unpartitioned_data &&
	    RadixPartitioning::RadixBitsOfPowerOfTwo(unpartitioned_data->PartitionCount()) == 0;

	radix_bits = radix_bits_p;
	if (reuse_partitioned_data) {
		if (partitioned_data->Count() != 0) {
			partitioned_data->Reset();
		}
		partitioned_data->ResetAppendState(state.partitioned_append_state,
		                                   TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
	} else {
		InitializePartitionedData();
	}
	if (radix_bits >= UNPARTITIONED_RADIX_BITS_THRESHOLD) {
		if (reuse_unpartitioned_data) {
			if (unpartitioned_data->Count() != 0) {
				unpartitioned_data->Reset();
			}
			unpartitioned_data->ResetAppendState(state.unpartitioned_append_state,
			                                     TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
		} else {
			InitializeUnpartitionedData();
		}
	} else {
		unpartitioned_data.reset();
	}

	count = 0;
	adaptivity.Reset();
	state.dict_state.dictionary_id = string();

	// Compute effective capacity based on the previous iteration's actual group count.
	// This avoids clearing a large pointer table (O(max_capacity)) when prior iterations
	// processed few rows. The HT will grow during the iteration if more groups are encountered.
	const auto effective_capacity = GetCapacityForCount(prev_count);
	if (!hash_map.IsSet() || capacity < effective_capacity) {
		Resize(effective_capacity);
	} else {
		// Logically shrink to effective_capacity: only zero the portion we need.
		// Entries beyond effective_capacity are unreachable (bitmask limits all accesses),
		// so leaving them uncleared is safe. The physical buffer is reused in place.
		capacity = effective_capacity;
		entries = reinterpret_cast<ht_entry_t *>(hash_map.get());
		bitmask = capacity - 1;
		ClearPointerTable();
	}
}
} // namespace duckdb
