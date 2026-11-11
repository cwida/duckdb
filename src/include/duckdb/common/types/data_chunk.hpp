//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/data_chunk.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {
struct DBConfig;
class Allocator;
class ClientContext;
class ExecutionContext;
class VectorCache;
class Serializer;
class Deserializer;
enum class DebugVerificationMode : uint8_t;

//!  A Data Chunk represents a set of vectors.
/*!
    The data chunk class is the intermediate representation used by the
   execution engine of DuckDB. It effectively represents a subset of a relation.
   It holds a set of vectors that all have the same length.

    DataChunk is initialized using the DataChunk::Initialize function by
   providing it with a vector of TypeIds for the Vector members. By default,
   this function will also allocate a chunk of memory in the DataChunk for the
   vectors and all the vectors will be referencing vectors to the data owned by
   the chunk. The reason for this behavior is that the underlying vectors can
   become referencing vectors to other chunks as well (i.e. in the case an
   operator does not alter the data, such as a Filter operator which only adds a
   selection vector).

    In addition to holding the data of the vectors, the DataChunk also owns the
   selection vector that underlying vectors can point to.
*/
class DataChunk {
public:
	//! Creates an empty DataChunk
	DUCKDB_API DataChunk();
	DUCKDB_API ~DataChunk();

	//! The vectors owned by the DataChunk.
	vector<Vector> data;

public:
	inline idx_t size() const {
		if (count.IsValid()) {
			return count.GetIndex();
		}
		return DeriveSize();
	}
	inline idx_t ColumnCount() const {
		return data.size();
	}
	//! The number of rows the vectors of this chunk were initialized to hold
	DUCKDB_API idx_t GetCapacity() const;
	//! Verify all child vectors have the expected cardinality
	void CheckCardinality(idx_t count_p);
	//! Sets the cardinality of all child vectors of this chunk
	void SetChildCardinality(idx_t count_p);
	//! Sets only the logical cardinality; child vector sizes must be synchronized separately.
	void SetCardinalityUnsafe(idx_t count_p) {
		this->count = count_p;
	}
	//! Deprecated: use SetChildCardinality instead.
	//! NOTE: this only sets the chunk's cardinality, it does NOT resize the child vectors (matching the historical
	//! behavior on main). Callers that mutate the child vectors directly (e.g. Vector::Append/SetValue) and then call
	//! SetCardinality rely on this - forwarding to SetChildCardinality would resize/overwrite their data.
	[[deprecated("Use CheckCardinality (preferred) or SetChildCardinality instead")]] DUCKDB_API void
	SetCardinality(idx_t count_p) {
		SetCardinalityUnsafe(count_p);
	}
	//! Deprecated: use SetChildCardinality instead
	[[deprecated("Use CheckCardinality (preferred) or SetChildCardinality instead")]] DUCKDB_API void
	SetCardinality(const DataChunk &chunk) {
		this->count = chunk.size();
	}

	DUCKDB_API Value GetValue(idx_t col_idx, idx_t index) const;
	[[deprecated("Use Vector::Append on data[col_idx] instead (or Vector::SetValue for write-at-index "
	             "semantics)")]] DUCKDB_API void
	SetValue(idx_t col_idx, idx_t index, const Value &val);

	//! Returns the uncompressed size of the data elements stored in this data chunk
	idx_t GetDataSize() const;
	//! Returns the size of the allocated data by this data chunk
	idx_t GetAllocationSize() const;

	//! Returns true if all vectors in the DataChunk are constant
	DUCKDB_API bool AllConstant() const;

	//! Set the DataChunk to reference another data chunk
	DUCKDB_API void Reference(DataChunk &chunk);
	//! Set the DataChunk to own the data of data chunk, destroying the other chunk in the process
	DUCKDB_API void Move(DataChunk &chunk);

	//! Initializes a DataChunk with the given types and without any vector data allocation.
	DUCKDB_API void InitializeEmpty(const vector<LogicalType> &types);

	//! Initializes typed storage, allocating payloads on first mutable access.
	//! An initialize mask can leave individual columns without storage.
	DUCKDB_API void Initialize(ClientContext &context, const vector<LogicalType> &types,
	                           idx_t capacity = STANDARD_VECTOR_SIZE);
	DUCKDB_API void Initialize(Allocator &allocator, const vector<LogicalType> &types,
	                           idx_t capacity = STANDARD_VECTOR_SIZE);
	DUCKDB_API void Initialize(ClientContext &context, const vector<LogicalType> &types, const vector<bool> &initialize,
	                           idx_t capacity = STANDARD_VECTOR_SIZE);
	DUCKDB_API void Initialize(Allocator &allocator, const vector<LogicalType> &types, const vector<bool> &initialize,
	                           idx_t capacity = STANDARD_VECTOR_SIZE);

	//! Append the other DataChunk to this one. The column count and types of
	//! the two DataChunks have to match exactly. Throws an exception if there
	//! is not enough space in the chunk and resize is not allowed.
	DUCKDB_API void Append(const DataChunk &other, VectorAppendMode append_mode = VectorAppendMode::ERROR_ON_NO_SPACE);
	DUCKDB_API void Append(const DataChunk &other, const SelectionVector &sel, idx_t sel_count,
	                       VectorAppendMode append_mode = VectorAppendMode::ERROR_ON_NO_SPACE);

	//! Destroy all data and columns owned by this DataChunk
	DUCKDB_API void Destroy();

	//! Copies the data from this chunk to another chunk.
	DUCKDB_API void Copy(DataChunk &other, idx_t offset = 0) const;
	DUCKDB_API void Copy(DataChunk &other, const SelectionVector &sel, const idx_t source_count,
	                     const idx_t offset = 0) const;

	//! Splits the DataChunk in two
	DUCKDB_API void Split(DataChunk &other, idx_t split_idx);

	//! Fuses a DataChunk onto the right of this one, and destroys the other. Inverse of Split.
	DUCKDB_API void Fuse(DataChunk &other);

	//! Makes this DataChunk reference the specified columns in the other DataChunk
	DUCKDB_API void ReferenceColumns(DataChunk &other, const vector<column_t> &column_ids);

	//! Turn all the vectors from the chunk into flat vectors
	DUCKDB_API void Flatten();

	// FIXME: this is DUCKDB_API, might need conversion back to regular unique ptr?
	DUCKDB_API unsafe_unique_array<UnifiedVectorFormat> ToUnifiedFormat();

	DUCKDB_API void Slice(const SelectionVector &sel_vector, idx_t count);

	//! Slice all Vectors from other.data[i] to data[i + 'col_offset']
	//! Turning all Vectors into Dictionary Vectors, using 'sel'
	DUCKDB_API void Slice(const DataChunk &other, const SelectionVector &sel, idx_t count, idx_t col_offset = 0);
	//! Slice all vectors from other.data from "offset..end"
	DUCKDB_API void Slice(const DataChunk &other, idx_t offset, idx_t end);

	//! Slice a DataChunk from "offset" to "offset + count"
	DUCKDB_API void Slice(idx_t offset, idx_t count);

	//! Clears the batch and restores owned storage, retaining reusable allocations.
	DUCKDB_API void Reset();

	DUCKDB_API void Serialize(Serializer &serializer, bool compressed_serialization = true) const;
	DUCKDB_API void Deserialize(Deserializer &source);

	//! Hashes the DataChunk to the target vector
	DUCKDB_API void Hash(Vector &result);
	//! Hashes specific vectors of the DataChunk to the target vector
	DUCKDB_API void Hash(vector<idx_t> &column_ids, Vector &result);

	//! Returns a list of types of the vectors of this data chunk
	DUCKDB_API vector<LogicalType> GetTypes() const;

	//! Converts this DataChunk to a printable string representation
	DUCKDB_API string ToString() const;
	DUCKDB_API void Print() const;

	DataChunk(const DataChunk &) = delete;

	//! Verify that the DataChunk is in a consistent, not corrupt state. DEBUG
	//! FUNCTION ONLY!
	DUCKDB_API void Verify(DatabaseInstance &db);
	DUCKDB_API void Verify(shared_ptr<DatabaseInstance> &db);
	DUCKDB_API void Verify(ClientContext &context);
	DUCKDB_API void Verify(optional_ptr<ClientContext> context);
	DUCKDB_API void Verify();

private:
	optional_idx count;

private:
	//! Vector caches, used to store data when ::Initialize is called
	vector<VectorCache> vector_caches;

private:
	void VerifyInternal(DebugVerificationMode mode, optional_ptr<DatabaseInstance> db);
	//! Derives the cardinality from the child vectors when no explicit count is set.
	//! Kept out-of-line so that ::size() inlines down to a load and a branch.
	DUCKDB_API idx_t DeriveSize() const;
};
} // namespace duckdb
