//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/aggregate_hashtable.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/row_operations/row_matcher.hpp"
#include "duckdb/common/types/row/partitioned_tuple_data.hpp"
#include "duckdb/execution/base_aggregate_hashtable.hpp"
#include "duckdb/execution/aggregate_ht_adaptivity_state.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/clustered_aggregate.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

class BlockHandle;
class GroupedAggregateHashTable;

struct FlushMoveState;

//! GroupedAggregateHashTable is a linear probing HT that is used for computing
//! aggregates
/*!
    GroupedAggregateHashTable is a HT that is used for computing aggregates. It takes
   as input the set of groups and the types of the aggregates to compute and
   stores them in the HT. It uses linear probing for collision resolution.
*/
struct AggregateHTScanState {
public:
	AggregateHTScanState() {
	}

	idx_t partition_idx = 0;
	TupleDataScanState scan_states;
};

//! Scratch shared by mutable append probes and task-local read-only lookups.
struct AggregateHTProbeState {
public:
	AggregateHTProbeState();

	Vector hashes;
	Vector ht_offsets;
	Vector hash_salts;
	Vector addresses;
	SelectionVector group_compare_vector;
	SelectionVector no_match_vector;
	DataChunk group_chunk;
	optional_ptr<const GroupedAggregateHashTable> owner;
	bool initialized = false;
};

//! Task-local scratch state for read-only group lookups.
struct AggregateHTLookupState : public AggregateHTProbeState {
public:
	AggregateHTLookupState();

	SelectionVector missing_vector;
	TupleDataChunkState chunk_state;
	RowMatcher row_matcher;
	vector<TupleDataGatherFunction> gather_functions;
};

class GroupedAggregateHashTable : public BaseAggregateHashTable {
public:
	GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types,
	                          vector<LogicalType> payload_types, const vector<BoundAggregateExpression *> &aggregates,
	                          idx_t initial_capacity = InitialCapacity(), idx_t radix_bits = 0,
	                          TupleDataValidityType group_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types,
	                          vector<LogicalType> payload_types, vector<AggregateObject> aggregates,
	                          idx_t initial_capacity = InitialCapacity(), idx_t radix_bits = 0,
	                          TupleDataValidityType group_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types,
	                          TupleDataValidityType group_validity = TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	~GroupedAggregateHashTable() override;

public:
	//! The hash table load factor, when a resize is triggered
	constexpr static double LOAD_FACTOR = 1.5;

	//! Get the layout of this HT
	shared_ptr<TupleDataLayout> GetLayoutPtr();
	const TupleDataLayout &GetLayout() const;
	//! Number of groups in the HT
	idx_t Count() const;
	//! Initial capacity of the HT
	static idx_t InitialCapacity();
	//! Capacity that can hold 'count' entries without resizing
	static idx_t GetCapacityForCount(idx_t count);
	//! Current capacity of the HT
	idx_t Capacity() const;
	//! Threshold at which to resize the HT
	idx_t ResizeThreshold() const;
	static idx_t ResizeThreshold(idx_t capacity);

	//! Add the given data to the HT, computing the aggregates grouped by the
	//! data in the group chunk. When resize = true, aggregates will not be
	//! computed but instead just assigned.
	idx_t AddChunk(DataChunk &groups, DataChunk &payload, const unsafe_vector<idx_t> &filter);
	idx_t AddChunk(DataChunk &groups, Vector &group_hashes, DataChunk &payload, const unsafe_vector<idx_t> &filter);
	idx_t AddChunk(DataChunk &groups, DataChunk &payload, AggregateType filter);
	using before_update_callback_t =
	    std::function<void(const Vector &group_addresses, const SelectionVector &new_groups, idx_t new_group_count)>;
	//! Adds a chunk and invokes a callback with stable row-start addresses before updating the aggregate states.
	idx_t AddChunk(DataChunk &groups, DataChunk &payload, AggregateType filter,
	               const before_update_callback_t &before_update);
	//! Adds a chunk and returns the stable row-start addresses and input indexes of newly created groups.
	idx_t AddChunkAndGetNewGroups(DataChunk &groups, DataChunk &payload, AggregateType filter,
	                              Vector &new_group_addresses, SelectionVector &new_groups_out);
	optional_idx TryAddCompressedGroups(DataChunk &groups, DataChunk &payload, const unsafe_vector<idx_t> &filter);
	optional_idx TryAddDictionaryGroups(DataChunk &groups, DataChunk &payload, const unsafe_vector<idx_t> &filter);
	optional_idx TryAddConstantGroups(DataChunk &groups, DataChunk &payload, const unsafe_vector<idx_t> &filter);

	//! Fetch the aggregates for specific groups from the HT and place them in the result
	void FetchAggregates(DataChunk &groups, DataChunk &result);

	void InitializeScan(AggregateHTScanState &scan_state);
	//! Scans group columns without reading or finalizing aggregate states.
	bool ScanGroups(AggregateHTScanState &scan_state, DataChunk &distinct_rows);
	bool Scan(AggregateHTScanState &scan_state, DataChunk &distinct_rows, DataChunk &payload_rows);

	//! Finds or creates groups in the hashtable using the specified group keys. The addresses vector will be filled
	//! with pointers to the groups in the hash table, and the new_groups selection vector will point to the newly
	//! created groups. The return value is the amount of newly created groups.
	idx_t FindOrCreateGroups(DataChunk &groups, Vector &group_hashes, Vector &addresses_out,
	                         SelectionVector &new_groups_out);
	idx_t FindOrCreateGroups(DataChunk &groups, Vector &addresses_out, SelectionVector &new_groups_out);
	void FindOrCreateGroups(DataChunk &groups, Vector &addresses_out);
	//! Finds existing groups without changing the hash table. Returns the number of matches and writes input-row
	//! indexes to found_groups_out. Matching row addresses are stored at their input-row indexes in state.addresses.
	idx_t LookupGroups(DataChunk &groups, AggregateHTLookupState &state, SelectionVector &found_groups_out) const;
	//! Gathers matched group values from state.addresses in the order specified by found_groups.
	void GatherGroups(AggregateHTLookupState &state, const SelectionVector &found_groups, idx_t found_count,
	                  DataChunk &result) const;
	//! Gathers group values from arbitrary row-start addresses.
	void GatherGroups(AggregateHTLookupState &state, Vector &addresses, const SelectionVector &groups,
	                  idx_t group_count, DataChunk &result) const;

	const PartitionedTupleData &GetPartitionedData() const;
	//! Retained tuple blocks, allocator metadata, aggregate arena, and pointer table
	idx_t GetSizeInBytes() const;
	unique_ptr<PartitionedTupleData> AcquirePartitionedData();
	void Abandon();
	void Repartition();
	shared_ptr<ArenaAllocator> GetAggregateAllocator();

	//! Resize an empty HT, or grow an HT with no abandoned groups.
	void Resize(idx_t size);
	//! Resets the pointer table of the HT to all 0's
	void ClearPointerTable();
	//! Set the radix bits for this HT
	void SetRadixBits(idx_t radix_bits);
	//! Get the radix bits for this HT
	idx_t GetRadixBits() const;
	//! Get the total number of tuples materialized currently in this HT
	idx_t GetMaterializedCount() const;
	//! Read-only adaptivity observations; mutations follow the HT's lifecycle.
	const AggregateHTAdaptivityState &GetAdaptivityState() const {
		return adaptivity;
	}
	//! Skip lookups until resuming at an empty pointer table
	void SkipLookups();
	//! Resume lookups after abandoning the previous pointer table.
	void ResumeLookups();
	//! Enable/disable HLL; a fresh sketch must start before inserting groups
	void EnableHLL(bool enable);

	//! Executes the filter(if any) and update the aggregates
	void Combine(GroupedAggregateHashTable &other);
	//! Combines the data into this HT - combined_chunks (if set) is incremented for every combined chunk
	void Combine(TupleDataCollection &other_data, optional_ptr<atomic<idx_t>> combined_chunks = nullptr);
	//! Reset the HT for a new execution while reusing internal allocations where possible
	void ResetForNewIteration(idx_t radix_bits);

private:
	ClientContext &context;
	//! Efficiently matches groups
	RowMatcher row_matcher;

	struct AggregateDictionaryState {
		AggregateDictionaryState();

		//! The current dictionary vector id (if any)
		string dictionary_id;
		DataChunk unique_values;
		Vector hashes;
		Vector new_dictionary_pointers;
		SelectionVector unique_entries;
		unique_ptr<Vector> dictionary_addresses;
		unsafe_unique_array<bool> found_entry;
		idx_t capacity = 0;
		//! Cumulative count of dict slots added to found_entry under the current id; the
		//! unique-entries walk is skipped once it reaches dict_size.
		idx_t resolved_count = 0;
		//! For the clustered-aggregate fast path: track whether every state pointer added to this
		//! dictionary shares the same high (64 - SLOT_GID_BITS) bits. The clustered path stores only
		//! the low SLOT_GID_BITS of each pointer, so if any two pointers differ in their high bits
		//! we cannot reuse the truncated form to disambiguate them and must fall back to scatter.
		//! address_high_bits starts at the sentinel ~0 ("no address seen yet"); a real high-bits
		//! value always has its low SLOT_GID_BITS cleared so ~0 cannot collide with a valid value.
		uint64_t address_high_bits = ~uint64_t(0);
		bool address_high_bits_uniform = true;
	};

	//! If we have this many or more radix bits, we use the unpartitioned data collection too
	static constexpr idx_t UNPARTITIONED_RADIX_BITS_THRESHOLD = 3;
	//! The number of radix bits to partition by
	idx_t radix_bits;
	//! The data of the HT
	unique_ptr<PartitionedTupleData> partitioned_data;
	unique_ptr<PartitionedTupleData> unpartitioned_data;
	//! Cache tuple allocations while the materialized groups and their storage stay unchanged.
	struct TupleSizeCache {
		optional_idx count;
		idx_t size = 0;
	};
	mutable TupleSizeCache tuple_size_cache;

	//! Predicates for matching groups (always ExpressionType::COMPARE_EQUAL)
	vector<ExpressionType> predicates;

	//! The number of groups in the HT
	idx_t count;
	//! The capacity of the HT. This can be increased using GroupedAggregateHashTable::Resize
	idx_t capacity;
	//! The hash map (pointer table) of the HT: allocated data and pointer into it
	HugeAllocation hash_map;
	ht_entry_t *entries;
	//! Offset of the hash column in the rows
	idx_t hash_offset;
	//! Bitmask for getting relevant bits from the hashes to determine the position
	hash_t bitmask;

	AggregateHTAdaptivityState adaptivity;

	//! The active arena allocator used by the aggregates for their internal state
	shared_ptr<ArenaAllocator> aggregate_allocator;
	//! Owning arena allocators that this HT has data from
	vector<shared_ptr<ArenaAllocator>> stored_allocators;

	//! Append state
	struct AggregateHTAppendState : public AggregateHTProbeState {
		explicit AggregateHTAppendState(ArenaAllocator &allocator);

		PartitionedTupleDataAppendState partitioned_append_state;
		PartitionedTupleDataAppendState unpartitioned_append_state;

		SelectionVector new_groups;
		AggregateDictionaryState dict_state;

		RowOperationsState row_state;
	} state;

	ClusteredAggrState clustered_state;

private:
	//! Disabled the copy constructor
	GroupedAggregateHashTable(const GroupedAggregateHashTable &) = delete;
	//! Destroy the HT
	void Destroy();
	void DestroyAggregateData(PartitionedTupleData &data, PartitionedTupleDataAppendState &append_state);

	//! Initializes the PartitionedTupleData
	void InitializePartitionedData();
	//! Initializes the PartitionedTupleData that only has 1 partition
	void InitializeUnpartitionedData();
	//! Apply bitmask to get the entry in the HT
	inline idx_t ApplyBitMask(hash_t hash) const;
	//! Reinserts tuples (triggered by Resize)
	void ReinsertTuples(PartitionedTupleData &data);

	void UpdateAggregates(DataChunk &payload, const unsafe_vector<idx_t> &filter, idx_t count,
	                      bool ht_offsets_valid = true);
	bool UpdateAggregatesClustered(DataChunk &payload, const unsafe_vector<idx_t> &filter, idx_t count,
	                               bool ht_offsets_valid);

	//! Does the actual group matching / creation
	idx_t FindOrCreateGroupsInternal(DataChunk &groups, Vector &group_hashes, Vector &addresses,
	                                 SelectionVector &new_groups,
	                                 optional_ptr<TupleDataChunkState> source_rows = nullptr);
	void InitializeLookupState(AggregateHTLookupState &lookup_state) const;

	//! Verify the pointer table of the HT
	void Verify();
};

} // namespace duckdb
