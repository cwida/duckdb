//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/row/tuple_data_states.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/sorting/sort_key.hpp"

namespace duckdb {

struct TupleDataChunkPart;

enum class TupleDataPinProperties : uint8_t {
	INVALID,
	//! Keeps all passed blocks pinned while scanning/iterating over the chunks (for both reading/writing)
	KEEP_EVERYTHING_PINNED,
	//! Unpins blocks after they are done (for both reading/writing)
	UNPIN_AFTER_DONE,
	//! Destroys blocks after they are done (for reading only)
	DESTROY_AFTER_DONE,
	//! Assumes all blocks are already pinned (for reading only)
	ALREADY_PINNED
};

//! Instead of an unordered_map from uint32 -> BufferHandle, we have a vector.
//! The lookup speed is OK since these maps should be very small.
//! The benefit of this is that we aren't doing any other heap allocations than the one vector.
//! For unordered_map, we would get a heap allocation for every inserted entry.
class buffer_handle_map_t {
	using iterator = unsafe_vector<pair<uint32_t, BufferHandle>>::iterator;

public:
	buffer_handle_map_t() {
	}

public:
	pair<iterator, bool> emplace(const uint32_t &id, BufferHandle &&handle) {
		D_ASSERT(find(id) == end()); // Should have been checked by the caller
		handles.emplace_back(id, std::move(handle));
		return make_pair(--end(), true);
	}

	iterator erase(const iterator &it) {
		return handles.erase(it);
	}

	iterator find(const uint32_t &id) {
		auto it = handles.begin();
		for (; it != handles.end(); it++) {
			if (it->first == id) {
				break;
			}
		}
		return it;
	}

	iterator begin() {
		return handles.begin();
	}

	iterator end() {
		return handles.end();
	}

	void clear() {
		handles.clear();
	}

	void acquire_handles(vector<BufferHandle> &pins) {
		for (auto &handle : handles) {
			pins.emplace_back(std::move(handle.second));
		}
		handles.clear();
	}

private:
	unsafe_vector<pair<uint32_t, BufferHandle>> handles;
};

struct TupleDataPinState {
	buffer_handle_map_t row_handles;
	buffer_handle_map_t heap_handles;
	TupleDataPinProperties properties = TupleDataPinProperties::INVALID;

	void Reset() {
		row_handles.clear();
		heap_handles.clear();
		properties = TupleDataPinProperties::INVALID;
	}
};

struct CombinedListData {
	CombinedListData() : combined_validity(STANDARD_VECTOR_SIZE) {
	}
	UnifiedVectorFormat combined_data;
	buffer_ptr<SelectionData> selection_data;
	list_entry_t combined_list_entries[STANDARD_VECTOR_SIZE];
	ValidityMask combined_validity;
};

struct TupleDataVectorFormat {
	const SelectionVector *original_sel;
	SelectionVector original_owned_sel;

	UnifiedVectorFormat unified;
	vector<TupleDataVectorFormat> children;
	unique_ptr<CombinedListData> combined_list_data;

	// Optional: only used for ArrayVector to fake being a list vector
	unsafe_unique_array<list_entry_t> array_list_entries;
};

struct TupleDataChunkState {
	vector<TupleDataVectorFormat> vector_data;
	vector<column_t> column_ids;

	Vector row_locations = Vector(LogicalType::POINTER);
	Vector heap_locations = Vector(LogicalType::POINTER);
	Vector heap_sizes = Vector(LogicalType::UBIGINT);

	optional_ptr<mutex> chunk_lock;

	SelectionVector utility = SelectionVector(STANDARD_VECTOR_SIZE);

	vector<unique_ptr<Vector>> cached_cast_vectors;
	vector<unique_ptr<VectorCache>> cached_cast_vector_cache;

	//! Re-usable arrays used while building buffer space
	unsafe_vector<reference<TupleDataChunkPart>> chunk_parts;
	unsafe_vector<pair<idx_t, idx_t>> chunk_part_indices;

	void ResetForScan() {
		column_ids.clear();
		chunk_lock = nullptr;
		cached_cast_vectors.clear();
		cached_cast_vector_cache.clear();
		chunk_parts.clear();
		chunk_part_indices.clear();
	}
};

struct SortKeyPayloadState {
	TupleDataChunkState &sort_key_chunk_state;
	SortKeyType sort_key_type;
	idx_t begin = 0;
	idx_t end = STANDARD_VECTOR_SIZE;
};

struct TupleDataAppendState {
	TupleDataPinState pin_state;
	TupleDataChunkState chunk_state;
};

struct TupleDataScanState {
	TupleDataPinState pin_state;
	TupleDataChunkState chunk_state;
	idx_t segment_index = DConstants::INVALID_INDEX;
	idx_t chunk_index = DConstants::INVALID_INDEX;

	void Reset() {
		pin_state.Reset();
		chunk_state.ResetForScan();
		segment_index = DConstants::INVALID_INDEX;
		chunk_index = DConstants::INVALID_INDEX;
	}
};

struct TupleDataParallelScanState {
	TupleDataScanState scan_state;
	mutex lock;
};

using TupleDataLocalScanState = TupleDataScanState;

} // namespace duckdb
