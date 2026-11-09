#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

template <bool IS_ENABLED>
struct MemorySafety {
#ifdef DEBUG
	// In DEBUG mode safety is always on
	static constexpr bool ENABLED = true;
#else
	static constexpr bool ENABLED = IS_ENABLED;
#endif
};

//! Reporters for a violated safety check. These are what the inline pointer and container accessors call when their
//! check fails. Building the exception - not the throw - is what pushes an accessor past the inliner's budget, so it
//! is kept out-of-line here and the accessors stay small enough to inline into their callers.
//! There is one reporter per message rather than one reporter taking the message: passing a string literal makes the
//! failing branch materialise the literal's address, which the machine outliner folds into a call, which forces a
//! frame setup, which in turn makes the branch large enough to be split off as a named `.cold` fragment. With no
//! literal to materialise the branch stays a bare tail call and no such fragment - nor its symbol - is emitted.
//! Arguments already living in registers, such as an index and a size, cost nothing; only literals do.
[[noreturn]] DUCKDB_API void ThrowNullUniquePtrDereference();
[[noreturn]] DUCKDB_API void ThrowNullSharedPtrDereference();
[[noreturn]] DUCKDB_API void ThrowOptionalPointerNotSet();
[[noreturn]] DUCKDB_API void ThrowNullArrayPtrConstruction();
[[noreturn]] DUCKDB_API void ThrowArrayPtrIteratorOutOfRange();

[[noreturn]] DUCKDB_API void ThrowVectorIndexOutOfBounds(idx_t index, idx_t size);
[[noreturn]] DUCKDB_API void ThrowVectorEraseOutOfBounds(idx_t index, idx_t size);
[[noreturn]] DUCKDB_API void ThrowDequeIndexOutOfBounds(idx_t index, idx_t size);
[[noreturn]] DUCKDB_API void ThrowArrayPtrIndexOutOfBounds(idx_t index, idx_t size);
[[noreturn]] DUCKDB_API void ThrowArrayPtrSubArrayOutOfBounds(idx_t offset, idx_t count, idx_t size);

[[noreturn]] DUCKDB_API void ThrowVectorBackOnEmpty();
[[noreturn]] DUCKDB_API void ThrowVectorPopBackOnEmpty();
[[noreturn]] DUCKDB_API void ThrowDequeFrontOnEmpty();
[[noreturn]] DUCKDB_API void ThrowDequeBackOnEmpty();
[[noreturn]] DUCKDB_API void ThrowQueueFrontOnEmpty();
[[noreturn]] DUCKDB_API void ThrowQueueBackOnEmpty();
[[noreturn]] DUCKDB_API void ThrowQueuePopOnEmpty();

} // namespace duckdb
