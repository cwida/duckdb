//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/chunk_collection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

//!  A ChunkCollection represents a set of DataChunks that all have the same
//!  types
/*!
    A ChunkCollection represents a set of DataChunks concatenated together in a
   list. Individual values of the collection can be iterated over using the
   iterator. It is also possible to iterate directly over the chunks for more
   direct access.
*/
class ChunkCollection {
public:
	ChunkCollection() : count(0) {
	}

	//! The amount of columns in the ChunkCollection
	DUCKDB_API vector<LogicalType> &Types() {
		return types;
	}
	const vector<LogicalType> &Types() const {
		return types;
	}

	//! The amount of rows in the ChunkCollection
	DUCKDB_API const idx_t &Count() const {
		return count;
	}

	//! The amount of columns in the ChunkCollection
	DUCKDB_API idx_t ColumnCount() const {
		return types.size();
	}

	//! Append a new DataChunk directly to this ChunkCollection
	DUCKDB_API void Append(DataChunk &new_chunk);

	//! Append another ChunkCollection directly to this ChunkCollection
	DUCKDB_API void Append(ChunkCollection &other);

	//! Merge is like Append but messes up the order and destroys the other collection
	DUCKDB_API void Merge(ChunkCollection &other);

	DUCKDB_API void Verify();

	//! Gets the value of the column at the specified index
	DUCKDB_API Value GetValue(idx_t column, idx_t index);
	//! Sets the value of the column at the specified index
	DUCKDB_API void SetValue(idx_t column, idx_t index, const Value &value);

	DUCKDB_API string ToString() const {
		return chunks.size() == 0 ? "ChunkCollection [ 0 ]"
		                          : "ChunkCollection [ " + std::to_string(count) + " ]: \n" + chunks[0]->ToString();
	}
	DUCKDB_API void Print();

	//! Gets a reference to the chunk at the given index
	DUCKDB_API DataChunk &GetChunkForRow(idx_t row_index) {
		return *chunks[LocateChunk(row_index)];
	}

	//! Gets a reference to the chunk at the given index
	DUCKDB_API DataChunk &GetChunk(idx_t chunk_index) {
		D_ASSERT(chunk_index < chunks.size());
		return *chunks[chunk_index];
	}
	const DataChunk &GetChunk(idx_t chunk_index) const {
		D_ASSERT(chunk_index < chunks.size());
		return *chunks[chunk_index];
	}

	DUCKDB_API const vector<unique_ptr<DataChunk>> &Chunks() {
		return chunks;
	}

	DUCKDB_API idx_t ChunkCount() const {
		return chunks.size();
	}

	DUCKDB_API void Reset() {
		count = 0;
		chunks.clear();
		types.clear();
	}

	DUCKDB_API unique_ptr<DataChunk> Fetch() {
		if (ChunkCount() == 0) {
			return nullptr;
		}

		auto res = move(chunks[0]);
		chunks.erase(chunks.begin() + 0);
		return res;
	}

	DUCKDB_API void Sort(vector<OrderType> &desc, vector<OrderByNullType> &null_order, idx_t result[]);
	//! Reorders the rows in the collection according to the given indices.
	DUCKDB_API void Reorder(idx_t order[]);

	//! Returns true if the ChunkCollections are equivalent
	DUCKDB_API bool Equals(ChunkCollection &other);

	//! Locates the chunk that belongs to the specific index
	DUCKDB_API idx_t LocateChunk(idx_t index) {
		idx_t result = index / STANDARD_VECTOR_SIZE;
		D_ASSERT(result < chunks.size());
		return result;
	}

	DUCKDB_API void Heap(vector<OrderType> &desc, vector<OrderByNullType> &null_order, idx_t heap[], idx_t heap_size);
	DUCKDB_API idx_t MaterializeHeapChunk(DataChunk &target, idx_t order[], idx_t start_offset, idx_t heap_size);

private:
	//! The total amount of elements in the collection
	idx_t count;
	//! The set of data chunks in the collection
	vector<unique_ptr<DataChunk>> chunks;
	//! The types of the ChunkCollection
	vector<LogicalType> types;
};
} // namespace duckdb
