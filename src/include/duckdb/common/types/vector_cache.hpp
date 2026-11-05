//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/vector_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
class Allocator;
class Vector;
class VectorCacheEntry;

//! The VectorCache holds cached vector data.
//! It enables re-using the same memory for different vectors.
class VectorCache {
public:
	//! Instantiate an empty vector cache.
	DUCKDB_API VectorCache();
	//! Instantiate a vector cache with the given type and capacity.
	DUCKDB_API VectorCache(Allocator &allocator, const LogicalType &type, const idx_t capacity = STANDARD_VECTOR_SIZE);
	~VectorCache();
	DUCKDB_API VectorCache(VectorCache &&other) noexcept;
	DUCKDB_API VectorCache &operator=(VectorCache &&other) noexcept;

private:
	unique_ptr<VectorCacheEntry> cache_entry;

public:
	void ResetFromCache(Vector &result) const;
	const LogicalType &GetType() const;
	//! The capacity of the cached vector, or an invalid index if the cache is empty
	optional_idx GetCapacity() const;
};

} // namespace duckdb
