//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/row/partitioned_tuple_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/fixed_size_map.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/perfect_map_set.hpp"
#include "duckdb/common/types/row/tuple_data_allocator.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"

namespace duckdb {

//! Local state for parallel partitioning
struct PartitionedTupleDataAppendState {
public:
	PartitionedTupleDataAppendState() : partition_indices(LogicalType::UBIGINT) {
	}

public:
	bool compute_reverse_partition_sel = false;

	Vector partition_indices;
	SelectionVector partition_sel;
	SelectionVector reverse_partition_sel;

	static constexpr idx_t MAP_THRESHOLD = 256;
	perfect_map_t<list_entry_t> partition_entries;
	fixed_size_map_t<list_entry_t> fixed_partition_entries;
	unsafe_vector<idx_t> partition_offsets;

	unsafe_vector<TupleDataPinState> partition_pin_states;
	TupleDataChunkState chunk_state;

	//! Utility Vector for when repartitioning and copying rows straight from one collection to another
	unique_ptr<Vector> utility_vector;

public:
	template <bool fixed>
	typename std::conditional<fixed, fixed_size_map_t<list_entry_t>, perfect_map_t<list_entry_t>>::type &GetMap() {
		throw NotImplementedException("PartitionedTupleDataAppendState::GetMap for boolean value");
	}

	optional_idx GetPartitionIndexIfSinglePartition(const bool use_fixed_size_map) {
		optional_idx result;
		if (use_fixed_size_map) {
			if (fixed_partition_entries.size() == 1) {
				result = fixed_partition_entries.begin().GetKey();
			}
		} else {
			if (partition_entries.size() == 1) {
				result = partition_entries.begin()->first;
			}
		}
		return result;
	}
};

template <>
inline perfect_map_t<list_entry_t> &PartitionedTupleDataAppendState::GetMap<false>() {
	return partition_entries;
}

template <>
inline fixed_size_map_t<list_entry_t> &PartitionedTupleDataAppendState::GetMap<true>() {
	return fixed_partition_entries;
}

enum class PartitionedTupleDataType : uint8_t {
	INVALID,
	//! Radix partitioning on a hash column
	RADIX,
	//! Partitioning on the value of an integer column with known bounds
	INTEGER
};

class PartitionedTupleDataRepartitionKeyTracker {
public:
	virtual ~PartitionedTupleDataRepartitionKeyTracker() = default;

	virtual void RepartitionChunk(TupleDataCollection &source_partition, TupleDataChunkState &source_chunk,
	                              PartitionedTupleDataAppendState &target_append, idx_t count) = 0;
};

//! PartitionedTupleData represents partitioned row data, which serves as an interface for different types of
//! partitioning, e.g., radix, hive
class PartitionedTupleData {
public:
	virtual ~PartitionedTupleData();

public:
	//! Get the layout of this PartitionedTupleData
	shared_ptr<TupleDataLayout> GetLayoutPtr() const;
	const TupleDataLayout &GetLayout() const;
	//! Get the partitioning type of this PartitionedTupleData
	PartitionedTupleDataType GetType() const;
	//! Initializes a local state for parallel partitioning that can be merged into this PartitionedTupleData
	void InitializeAppendState(PartitionedTupleDataAppendState &state,
	                           TupleDataPinProperties properties = TupleDataPinProperties::UNPIN_AFTER_DONE) const;
	//! Reuses an existing append state for a new iteration - faster than InitializeAppendState when the
	//! append state already has pre-allocated buffers (selection vectors, partition pin states, chunk state)
	void ResetAppendState(PartitionedTupleDataAppendState &state,
	                      TupleDataPinProperties properties = TupleDataPinProperties::UNPIN_AFTER_DONE) const;
	//! Appends a DataChunk to this PartitionedTupleData
	void Append(PartitionedTupleDataAppendState &state, DataChunk &input,
	            const SelectionVector &append_sel = *FlatVector::IncrementalSelectionVector(),
	            const idx_t append_count = DConstants::INVALID_INDEX);
	//! Appends a DataChunk to this PartitionedTupleData
	//! - ToUnifiedFormat has already been called
	void AppendUnified(PartitionedTupleDataAppendState &state, DataChunk &input,
	                   const SelectionVector &append_sel = *FlatVector::IncrementalSelectionVector(),
	                   const idx_t append_count = DConstants::INVALID_INDEX);
	//! Appends rows to this PartitionedTupleData
	void Append(PartitionedTupleDataAppendState &state, TupleDataChunkState &input, const idx_t count,
	            const SelectionVector &append_sel = *FlatVector::IncrementalSelectionVector());
	//! Flushes any remaining data in the append state into this PartitionedTupleData
	void FlushAppendState(PartitionedTupleDataAppendState &state);
	//! Combine another PartitionedTupleData into this PartitionedTupleData
	void Combine(PartitionedTupleData &other);
	//! Resets this PartitionedTupleData
	void Reset();
	//! Repartition this PartitionedTupleData into the new PartitionedTupleData
	void Repartition(ClientContext &context, PartitionedTupleData &new_partitioned_data,
	                 optional_ptr<PartitionedTupleDataRepartitionKeyTracker> key_tracker = nullptr);
	//! Unpins the data
	void Unpin();
	//! Get the partitions in this PartitionedTupleData
	unsafe_vector<unique_ptr<TupleDataCollection>> &GetPartitions();
	//! Get the data of this PartitionedTupleData as a single unpartitioned TupleDataCollection
	unique_ptr<TupleDataCollection> GetUnpartitioned();
	//! Get the count of this PartitionedTupleData
	idx_t Count() const;
	//! Get the size (in bytes) of this PartitionedTupleData
	idx_t SizeInBytes() const;
	//! Buffer-manager memory charged for row and heap blocks, plus allocator metadata
	idx_t GetAllocatedSizeInBytes() const;
	//! Get the number of partitions of this PartitionedTupleData
	idx_t PartitionCount() const;
	//! Get the count and size of the largest partition
	void GetSizesAndCounts(vector<idx_t> &partition_sizes, vector<idx_t> &partition_counts) const;
	//! Converts this PartitionedTupleData to a string representation
	string ToString();
	//! Prints the string representation of this PartitionedTupleData
	void Print();

protected:
	//===--------------------------------------------------------------------===//
	// Partitioning type implementation interface
	//===--------------------------------------------------------------------===//
	//! Initialize a PartitionedTupleDataAppendState for this type of partitioning: by default the per-partition pin
	//! states, the chunk state and the map storage for the current partitions (optional)
	virtual void InitializeAppendStateInternal(PartitionedTupleDataAppendState &state,
	                                           TupleDataPinProperties properties) const;
	//! Compute the partition indices for this type of partitioning for the input DataChunk and store them in the
	//! `partition_data` of the local state. If this type creates partitions on the fly (for, e.g., hive), this
	//! function is also in charge of creating new partitions and mapping the input data to a partition index
	virtual void ComputePartitionIndices(PartitionedTupleDataAppendState &state, DataChunk &input,
	                                     const SelectionVector &append_sel, const idx_t append_count) {
		throw NotImplementedException("ComputePartitionIndices for this type of PartitionedTupleData");
	}
	//! Compute partition indices from rows (similar to function above)
	virtual void ComputePartitionIndices(Vector &row_locations, idx_t append_count, Vector &partition_indices,
	                                     unique_ptr<Vector> &utility_vector, const SelectionVector &append_sel) const {
		throw NotImplementedException("ComputePartitionIndices for this type of PartitionedTupleData");
	}
	//! Maximum partition index (optional)
	virtual idx_t MaxPartitionIndex() const {
		return DConstants::INVALID_INDEX;
	}

	//! Finalize states while repartitioning - useful for unpinning blocks that are no longer needed (optional)
	virtual void RepartitionFinalizeStates(PartitionedTupleData &old_partitioned_data,
	                                       PartitionedTupleData &new_partitioned_data,
	                                       PartitionedTupleDataAppendState &state, idx_t finished_partition_idx) const {
	}

protected:
	//! PartitionedTupleData can only be instantiated by derived classes
	PartitionedTupleData(PartitionedTupleDataType type, BufferManager &buffer_manager,
	                     shared_ptr<TupleDataLayout> &layout_ptr, MemoryTag tag, QueryContext context = QueryContext());
	PartitionedTupleData(PartitionedTupleData &other);

	//! Whether to use fixed size map or regular map
	bool UseFixedSizeMap() const;
	//! Builds a selection vector in the Append state for the partitions
	//! - returns true if everything belongs to the same partition - stores partition index in single_partition_idx
	void BuildPartitionSel(PartitionedTupleDataAppendState &state, const SelectionVector &append_sel,
	                       const idx_t append_count) const;
	template <bool FIXED>
	static void BuildPartitionSel(PartitionedTupleDataAppendState &state, const SelectionVector &append_sel,
	                              const idx_t append_count, const idx_t max_partition_idx);
	//! Builds out the buffer space in the partitions
	void BuildBufferSpace(PartitionedTupleDataAppendState &state);
	template <bool FIXED>
	void BuildBufferSpace(PartitionedTupleDataAppendState &state);
	//! Create a collection for a specific a partition
	unique_ptr<TupleDataCollection> CreatePartitionCollection() {
		return make_uniq<TupleDataCollection>(buffer_manager, layout_ptr, tag, stl_allocator, context);
	}
	//! Verify count/data size of this PartitionedTupleData
	void Verify() const;

protected:
	PartitionedTupleDataType type;

	BufferManager &buffer_manager;
	//! The query context, used to attribute eviction/spill I/O of the partitions to the query
	QueryContext context;
	shared_ptr<ArenaAllocator> stl_allocator;

	shared_ptr<TupleDataLayout> layout_ptr;
	const TupleDataLayout &layout;

	const MemoryTag tag;

	idx_t count;
	idx_t data_size;

	mutex lock;
	unsafe_vector<unique_ptr<TupleDataCollection>> partitions;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
