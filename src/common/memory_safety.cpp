#include "duckdb/common/memory_safety.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

void ThrowNullUniquePtrDereference() {
	throw InternalException("Attempted to dereference unique_ptr that is NULL!");
}

void ThrowNullSharedPtrDereference() {
	throw InternalException("Attempted to dereference shared_ptr that is NULL!");
}

void ThrowOptionalPointerNotSet() {
	throw InternalException("Attempting to dereference an optional pointer that is not set");
}

void ThrowNullArrayPtrConstruction() {
	throw InternalException("Attempted to construct an array_ptr from a NULL pointer");
}

void ThrowArrayPtrIteratorOutOfRange() {
	throw InternalException("array_ptr iterator dereferenced while iterator is out of range");
}

void ThrowVectorIndexOutOfBounds(idx_t index, idx_t size) {
	throw InternalException("Attempted to access index %lld within vector of size %lld", index, size);
}

void ThrowVectorEraseOutOfBounds(idx_t index, idx_t size) {
	throw InternalException("Can't remove offset %d from vector of size %d", index, size);
}

void ThrowDequeIndexOutOfBounds(idx_t index, idx_t size) {
	throw InternalException("Attempted to access index %lld within deque of size %lld", index, size);
}

void ThrowArrayPtrIndexOutOfBounds(idx_t index, idx_t size) {
	throw InternalException("Attempted to access index %lld within array_ptr of size %lld", index, size);
}

void ThrowArrayPtrSubArrayOutOfBounds(idx_t offset, idx_t count, idx_t size) {
	throw InternalException(
	    "Attempted to construct a sub-array at offset %lld with size %lld from array_ptr of size %lld", offset, count,
	    size);
}

void ThrowVectorBackOnEmpty() {
	throw InternalException("'back' called on an empty vector!");
}

void ThrowVectorPopBackOnEmpty() {
	throw InternalException("'pop_back' called on an empty vector!");
}

void ThrowDequeFrontOnEmpty() {
	throw InternalException("'front' called on an empty deque!");
}

void ThrowDequeBackOnEmpty() {
	throw InternalException("'back' called on an empty deque!");
}

void ThrowQueueFrontOnEmpty() {
	throw InternalException("'front' called on an empty queue!");
}

void ThrowQueueBackOnEmpty() {
	throw InternalException("'back' called on an empty queue!");
}

void ThrowQueuePopOnEmpty() {
	throw InternalException("'pop' called on an empty queue!");
}

} // namespace duckdb
