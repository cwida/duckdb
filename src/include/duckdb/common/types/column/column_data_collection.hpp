//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/column/column_data_collection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection_iterators.hpp"

namespace duckdb {

class BufferManager;
class BlockHandle;
class ClientContext;
struct ColumnDataCopyFunction;
class ColumnDataAllocator;
class ColumnDataCollection;
class ColumnDataCollectionSegment;
class ColumnDataRowCollection;

//! The ColumnDataCollection represents a set of (buffer-managed) data stored in columnar format
//! It is efficient to read and scan
//! A collection with no columns still stores row counts.
//! The position of a chunk within a ColumnDataCollection
struct ColumnDataChunkPosition {
	idx_t segment_index = 0;
	idx_t chunk_index = 0;
	//! The number of rows before the chunk
	idx_t row_offset = 0;
};

class ColumnDataCollection {
public:
	//! Constructs an in-memory column data collection from an allocator
	DUCKDB_API ColumnDataCollection(Allocator &allocator, vector<LogicalType> types);
	//! Constructs an empty (but valid) in-memory column data collection from an allocator
	DUCKDB_API explicit ColumnDataCollection(Allocator &allocator);
	//! Constructs a buffer-managed column data collection
	DUCKDB_API
	ColumnDataCollection(BufferManager &buffer_manager, vector<LogicalType> types,
	                     ColumnDataCollectionLifetime lifetime = ColumnDataCollectionLifetime::REGULAR);
	//! Constructs either an in-memory or a buffer-managed column data collection
	DUCKDB_API
	ColumnDataCollection(ClientContext &context, vector<LogicalType> types,
	                     ColumnDataAllocatorType type = ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR,
	                     ColumnDataCollectionLifetime lifetime = ColumnDataCollectionLifetime::REGULAR);
	//! Creates a column data collection that inherits the blocks to write to. This allows blocks to be shared
	//! between multiple column data collections and prevents wasting space.
	//! Note that after one CDC inherits blocks from another, the other
	//! cannot be written to anymore (i.e. we take ownership of the half-written blocks).
	DUCKDB_API ColumnDataCollection(ColumnDataCollection &parent);
	DUCKDB_API ColumnDataCollection(shared_ptr<ColumnDataAllocator> allocator, vector<LogicalType> types);
	DUCKDB_API ~ColumnDataCollection();

public:
	//! The types of columns in the ColumnDataCollection
	vector<LogicalType> &Types() {
		return types;
	}
	const vector<LogicalType> &Types() const {
		return types;
	}

	//! The amount of rows in the ColumnDataCollection
	const idx_t &Count() const {
		return count;
	}

	//! The amount of columns in the ColumnDataCollection
	idx_t ColumnCount() const {
		return types.size();
	}

	//! The size (in bytes) of this ColumnDataCollection
	idx_t SizeInBytes() const;
	//! The allocation size (in bytes) of this ColumnDataCollection - this property is cached
	idx_t AllocationSize() const;
	//! Sets the partition index of this ColumnDataCollection
	void SetPartitionIndex(idx_t index);

	//! Get the allocator
	DUCKDB_API Allocator &GetAllocator() const;

	//! Initializes an Append state - useful for optimizing many appends made to the same column data collection
	DUCKDB_API void InitializeAppend(ColumnDataAppendState &state);
	//! Append a DataChunk to this ColumnDataCollection using the specified append state
	DUCKDB_API void Append(ColumnDataAppendState &state, DataChunk &new_chunk);

	//! Initializes a chunk with the correct types that can be used to call Scan
	DUCKDB_API void InitializeScanChunk(DataChunk &chunk) const;
	DUCKDB_API void InitializeScanChunk(Allocator &allocator, DataChunk &chunk) const;
	//! Initializes a chunk with the correct types for a given scan state
	DUCKDB_API void InitializeScanChunk(ColumnDataScanState &state, DataChunk &chunk) const;
	//! Initializes a Scan state for scanning all columns
	DUCKDB_API void
	InitializeScan(ColumnDataScanState &state,
	               ColumnDataScanProperties properties = ColumnDataScanProperties::ALLOW_ZERO_COPY) const;
	//! Initializes a Scan state for scanning a subset of the columns
	DUCKDB_API void
	InitializeScan(ColumnDataScanState &state, vector<column_t> column_ids,
	               ColumnDataScanProperties properties = ColumnDataScanProperties::ALLOW_ZERO_COPY) const;
	//! Initialize a parallel scan over the column data collection over all columns
	DUCKDB_API void
	InitializeScan(ColumnDataParallelScanState &state,
	               ColumnDataScanProperties properties = ColumnDataScanProperties::ALLOW_ZERO_COPY) const;
	//! Initialize a parallel scan over the column data collection over a subset of the columns
	DUCKDB_API void
	InitializeScan(ColumnDataParallelScanState &state, vector<column_t> column_ids,
	               ColumnDataScanProperties properties = ColumnDataScanProperties::ALLOW_ZERO_COPY) const;
	//! Scans a DataChunk from the ColumnDataCollection
	DUCKDB_API bool Scan(ColumnDataScanState &state, DataChunk &result) const;
	//! Scans a DataChunk from the ColumnDataCollection
	DUCKDB_API bool Scan(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate, DataChunk &result) const;

	//! Append a DataChunk directly to this ColumnDataCollection - calls InitializeAppend and Append internally
	DUCKDB_API void Append(DataChunk &new_chunk);
	//! Appends a copy of another ColumnDataCollection to this
	DUCKDB_API void Append(const ColumnDataCollection &other);

	//! Appends the other ColumnDataCollection to this, destroying the other data collection
	DUCKDB_API void Combine(ColumnDataCollection &other);
	//! Swaps the contents of two compatible ColumnDataCollections
	DUCKDB_API void Swap(ColumnDataCollection &other);

	DUCKDB_API void Verify();

	DUCKDB_API string ToString() const;
	DUCKDB_API void Print() const;

	DUCKDB_API void Reset();
	DUCKDB_API void ResetForReuse();

	//! Returns the number of data chunks present in the ColumnDataCollection
	DUCKDB_API idx_t ChunkCount() const;
	//! Fetch an individual chunk from the ColumnDataCollection
	DUCKDB_API void FetchChunk(idx_t chunk_idx, DataChunk &result) const;

	//! Constructs a class that can be iterated over to fetch individual chunks
	//! Iterating over this is syntactic sugar over just calling Scan
	DUCKDB_API ColumnDataChunkIterationHelper Chunks() const;
	//! Constructs a class that can be iterated over to fetch individual chunks
	//! Only the column indexes specified in the column_ids list are scanned
	DUCKDB_API ColumnDataChunkIterationHelper Chunks(vector<column_t> column_ids) const;

	//! Constructs a class that can be iterated over to fetch individual rows
	//! Note that row iteration is slow, and the `.Chunks()` method should be used instead
	DUCKDB_API ColumnDataRowIterationHelper Rows() const;

	//! Returns a materialized set of all of the rows in the column data collection
	//! Note that usage of this is slow - avoid using this unless the amount of rows is small, or if you do not care
	//! about performance
	DUCKDB_API ColumnDataRowCollection GetRows() const;

	//! Materializes every row on each call, so read many values through GetRows instead
	DUCKDB_API Value GetValue(idx_t column, idx_t index) const;

	//! Compare two column data collections to another. If they are equal according to result equality rules,
	//! return true. That means null values are equal, and approx equality is used for floating point values.
	//! If they are not equal, return false and fill in the error message.
	static bool ResultEquals(const ColumnDataCollection &left, const ColumnDataCollection &right, string &error_message,
	                         bool ordered = false);

	//! Obtains the next scan index to scan from
	bool NextScanIndex(ColumnDataScanState &state, idx_t &chunk_index, idx_t &segment_index, idx_t &row_index) const;
	//! Obtains the previous scan index to scan from
	bool PrevScanIndex(ColumnDataScanState &state, idx_t &chunk_index, idx_t &segment_index, idx_t &row_index) const;
	//! Scans at the indices (obtained from NextScanIndex)
	void ScanAtIndex(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate, DataChunk &result,
	                 idx_t chunk_index, idx_t segment_index, idx_t row_index) const;

	//! Seeks to the chunk _containing_ the row. Returns false if it is past the end.
	//! Note that the returned chunk will likely not be aligned to the given row
	//! but the scan state will provide the actual range
	bool Seek(idx_t row_idx, ColumnDataScanState &state, DataChunk &result) const;
	//! Positions an initialized scan so that the next Scan reads the chunk at the given position
	void SeekScan(ColumnDataScanState &state, const ColumnDataChunkPosition &position) const;

	//! Initialize the column data collection
	void Initialize(vector<LogicalType> types);

	//! Get references to the string heaps in this ColumnDataCollection
	vector<shared_ptr<StringHeap>> GetHeapReferences();
	//! Get the allocator type of this ColumnDataCollection
	ColumnDataAllocatorType GetAllocatorType() const;
	//! Get the buffer manager of the allocator
	BufferManager &GetBufferManager() const;

	//! Get a vector of the segments in this ColumnDataCollection
	const vector<unique_ptr<ColumnDataCollectionSegment>> &GetSegments() const;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<ColumnDataCollection> Deserialize(Deserializer &deserializer);

private:
	//! Creates a new segment within the ColumnDataCollection
	void CreateSegment();

	static ColumnDataCopyFunction GetCopyFunction(const LogicalType &type);

private:
	//! The Column Data Allocator
	buffer_ptr<ColumnDataAllocator> allocator;
	//! The types of the stored entries
	vector<LogicalType> types;
	//! The number of entries stored in the column data collection
	idx_t count;
	//! The data segments of the column data collection
	vector<unique_ptr<ColumnDataCollectionSegment>> segments;
	//! The set of copy functions
	vector<ColumnDataCopyFunction> copy_functions;
	//! When the column data collection is marked as finished - new tuples can no longer be appended to it
	bool finished_append;
	//! Partition index (optional, if partitioned)
	optional_idx partition_index;
};

//! Walks the chunks of a ColumnDataCollection in storage order: exposes the position, the row count and the
//! allocation size of every chunk, e.g. to plan scans over the collection
class ColumnDataChunkCursor {
public:
	explicit ColumnDataChunkCursor(const ColumnDataCollection &collection);

public:
	//! Moves to the next chunk; returns false once past the last chunk
	bool Next();
	//! The position of the current chunk
	const ColumnDataChunkPosition &Position() const {
		return position;
	}
	//! The number of rows in the current chunk
	idx_t Count() const;
	//! The allocated bytes of the current chunk
	idx_t AllocationSize() const;
	//! The allocated bytes of selected columns of the current chunk
	idx_t AllocationSize(const vector<column_t> &column_ids) const;

private:
	const ColumnDataCollection &collection;
	ColumnDataChunkPosition position;
	bool started = false;
};

//! The ColumnDataRowCollection represents a set of materialized rows, as obtained from the ColumnDataCollection
class ColumnDataRowCollection {
public:
	DUCKDB_API explicit ColumnDataRowCollection(
	    const ColumnDataCollection &collection,
	    ColumnDataScanProperties properties = ColumnDataScanProperties::DISALLOW_ZERO_COPY);

public:
	DUCKDB_API Value GetValue(idx_t column, idx_t index) const;

public:
	// container API
	bool empty() const {     // NOLINT: match stl API
		return rows.empty(); // NOLINT
	}
	idx_t size() const { // NOLINT: match stl API
		return rows.size();
	}

	DUCKDB_API ColumnDataRow &operator[](idx_t i);
	DUCKDB_API const ColumnDataRow &operator[](idx_t i) const;

	vector<ColumnDataRow>::iterator begin() { // NOLINT: match stl API
		return rows.begin();
	}
	vector<ColumnDataRow>::iterator end() { // NOLINT: match stl API
		return rows.end();
	}
	vector<ColumnDataRow>::const_iterator cbegin() const { // NOLINT: match stl API
		return rows.cbegin();
	}
	vector<ColumnDataRow>::const_iterator cend() const { // NOLINT: match stl API
		return rows.cend();
	}
	vector<ColumnDataRow>::const_iterator begin() const { // NOLINT: match stl API
		return rows.begin();
	}
	vector<ColumnDataRow>::const_iterator end() const { // NOLINT: match stl API
		return rows.end();
	}

private:
	vector<ColumnDataRow> rows;
	vector<unique_ptr<DataChunk>> chunks;
	ColumnDataScanState scan_state;
};

} // namespace duckdb
