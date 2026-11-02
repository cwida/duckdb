//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/index_storage_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enums/constraint_check_mode.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"

namespace duckdb {

//! Information to serialize a FixedSizeAllocator, which holds the index data
struct FixedSizeAllocatorInfo {
	idx_t segment_size;
	vector<idx_t> buffer_ids;
	vector<BlockPointer> block_pointers;
	vector<idx_t> segment_counts;
	vector<idx_t> allocation_sizes;
	vector<idx_t> buffers_with_free_space;
	//! Transient block handles used to initialize read-only entities (i.e., index entries during WAL replay)
	//! These are transient runtime state and are never serialized.
	shared_ptr<vector<shared_ptr<BlockHandle>>> transient_block_handles;

	void Serialize(Serializer &serializer) const;
	static FixedSizeAllocatorInfo Deserialize(Deserializer &deserializer);
};

//! Information to serialize an index buffer to the WAL
struct IndexBufferInfo {
	IndexBufferInfo(data_ptr_t buffer_ptr, const idx_t allocation_size)
	    : buffer_ptr(buffer_ptr), allocation_size(allocation_size) {
	}

	data_ptr_t buffer_ptr;
	idx_t allocation_size;
};

//! Index (de)serialization information.
struct IndexStorageInfo {
	IndexStorageInfo() {};
	explicit IndexStorageInfo(const Identifier &name) : name(name) {};

	//! Disable copy constructor and copy assignment, this type's lifetime is explicitly managed.
	IndexStorageInfo(const IndexStorageInfo &) = delete;
	IndexStorageInfo &operator=(const IndexStorageInfo &) = delete;

	IndexStorageInfo(IndexStorageInfo &&) = default;
	IndexStorageInfo &operator=(IndexStorageInfo &&) = default;

	//! The name.
	Identifier name;
	//! The storage root.
	idx_t root;
	//! Any index specialization can provide additional key-Value settings via this map.
	case_insensitive_map_t<Value> options;
	//! Serialization information for fixed-size allocator memory.
	vector<FixedSizeAllocatorInfo> allocator_infos;

	//! Contains all buffer pointers and their allocation size for serializing to the WAL.
	//! First dimension: All fixed-size allocators.
	//! Second dimension: The buffers of each fixed-size allocator.
	vector<vector<IndexBufferInfo>> buffers;

	//! The root block pointer of the index. Necessary to support older storage files.
	BlockPointer root_block_ptr;

	//! Returns true, if IndexStorageInfo holds information to deserialize an index.
	//! Note that the name can be misleading - any index that is empty (no nodes, etc.) might
	//! also have neither a root_block_ptr nor allocator_infos.
	//! Ensure that your index constructor initializes an empty index correctly without the
	//! need for these fields.
	bool IsValid() const {
		return root_block_ptr.IsValid() || !allocator_infos.empty();
	}

	void Serialize(Serializer &serializer) const;
	static IndexStorageInfo Deserialize(Deserializer &deserializer);
};

//! Additional index information for tables
struct IndexInfo {
	bool is_unique;
	bool is_primary;
	bool is_foreign;
	ConstraintCheckMode check_mode = ConstraintCheckMode::DEFAULT;
	unordered_set<column_t> column_set;
};

} // namespace duckdb
